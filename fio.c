/*
 * fio - the flexible io tester
 *
 * Copyright (C) 2005 Jens Axboe <axboe@suse.de>
 * Copyright (C) 2006 Jens Axboe <axboe@kernel.dk>
 *
 * The license below covers all files distributed with fio unless otherwise
 * noted in the file itself.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <limits.h>
#include <signal.h>
#include <time.h>
#include <locale.h>
#include <assert.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/mman.h>

#include "fio.h"
#include "hash.h"
#include "smalloc.h"
#include "verify.h"
#include "trim.h"
#include "diskutil.h"
#include "cgroup.h"
#include "profile.h"
#include "lib/rand.h"
#include "memalign.h"
#include "server.h"

unsigned long page_mask;
unsigned long page_size;

#define PAGE_ALIGN(buf)	\
	(char *) (((unsigned long) (buf) + page_mask) & ~page_mask)

int groupid = 0;
unsigned int thread_number = 0;
unsigned int nr_process = 0;
unsigned int nr_thread = 0;
int shm_id = 0;
int temp_stall_ts;
unsigned long done_secs = 0;

/*
 * Just expose an empty list, if the OS does not support disk util stats
 */
#ifndef FIO_HAVE_DISK_UTIL
FLIST_HEAD(disk_list);
#endif

static struct fio_mutex *startup_mutex;
static struct fio_mutex *writeout_mutex;
static volatile int fio_abort;
static int exit_value;
static pthread_t gtod_thread;
static pthread_t disk_util_thread;
static struct flist_head *cgroup_list;
static char *cgroup_mnt;

unsigned long arch_flags = 0;

struct io_log *agg_io_log[2];

#define JOB_START_TIMEOUT	(5 * 1000)

static const char *fio_os_strings[os_nr] = {
	"Invalid",
	"Linux",
	"AIX",
	"FreeBSD",
	"HP-UX",
	"OSX",
	"NetBSD",
	"Solaris",
	"Windows"
};

static const char *fio_arch_strings[arch_nr] = {
	"Invalid",
	"x86-64",
	"x86",
	"ppc",
	"ia64",
	"s390",
	"alpha",
	"sparc",
	"sparc64",
	"arm",
	"sh",
	"hppa",
	"generic"
};

const char *fio_get_os_string(int nr)
{
	if (nr < os_nr)
		return fio_os_strings[nr];

	return NULL;
}

const char *fio_get_arch_string(int nr)
{
	if (nr < arch_nr)
		return fio_arch_strings[nr];

	return NULL;
}

void td_set_runstate(struct thread_data *td, int runstate)
{
	if (td->runstate == runstate)
		return;

	dprint(FD_PROCESS, "pid=%d: runstate %d -> %d\n", (int) td->pid,
						td->runstate, runstate);
	td->runstate = runstate;
}

void fio_terminate_threads(int group_id)
{
	struct thread_data *td;
	int i;

	dprint(FD_PROCESS, "terminate group_id=%d\n", group_id);

	for_each_td(td, i) {
		if (group_id == TERMINATE_ALL || groupid == td->groupid) {
			dprint(FD_PROCESS, "setting terminate on %s/%d\n",
						td->o.name, (int) td->pid);
			td->terminate = 1;
			td->o.start_delay = 0;

			/*
			 * if the thread is running, just let it exit
			 */
			if (!td->pid)
				continue;
			else if (td->runstate < TD_RAMP)
				kill(td->pid, SIGTERM);
			else {
				struct ioengine_ops *ops = td->io_ops;

				if (ops && (ops->flags & FIO_SIGTERM))
					kill(td->pid, SIGTERM);
			}
		}
	}
}

static void sig_int(int sig)
{
	if (threads) {
		if (is_backend)
			fio_server_got_signal(sig);
		else {
			log_info("\nfio: terminating on signal %d\n", sig);
			fflush(stdout);
			exit_value = 128;
		}

		fio_terminate_threads(TERMINATE_ALL);
	}
}

static void *disk_thread_main(void *data)
{
	fio_mutex_up(startup_mutex);

	while (threads) {
		usleep(DISK_UTIL_MSEC * 1000);
		if (!threads)
			break;
		update_io_ticks();

		if (!is_backend)
			print_thread_status();
	}

	return NULL;
}

static int create_disk_util_thread(void)
{
	int ret;

	ret = pthread_create(&disk_util_thread, NULL, disk_thread_main, NULL);
	if (ret) {
		log_err("Can't create disk util thread: %s\n", strerror(ret));
		return 1;
	}

	ret = pthread_detach(disk_util_thread);
	if (ret) {
		log_err("Can't detatch disk util thread: %s\n", strerror(ret));
		return 1;
	}

	dprint(FD_MUTEX, "wait on startup_mutex\n");
	fio_mutex_down(startup_mutex);
	dprint(FD_MUTEX, "done waiting on startup_mutex\n");
	return 0;
}

static void set_sig_handlers(void)
{
	struct sigaction act;

	memset(&act, 0, sizeof(act));
	act.sa_handler = sig_int;
	act.sa_flags = SA_RESTART;
	sigaction(SIGINT, &act, NULL);

	memset(&act, 0, sizeof(act));
	act.sa_handler = sig_int;
	act.sa_flags = SA_RESTART;
	sigaction(SIGTERM, &act, NULL);

	if (is_backend) {
		memset(&act, 0, sizeof(act));
		act.sa_handler = sig_int;
		act.sa_flags = SA_RESTART;
		sigaction(SIGPIPE, &act, NULL);
	}
}

/*
 * Check if we are above the minimum rate given.
 */
static int __check_min_rate(struct thread_data *td, struct timeval *now,
			    enum fio_ddir ddir)
{
	unsigned long long bytes = 0;
	unsigned long iops = 0;
	unsigned long spent;
	unsigned long rate;
	unsigned int ratemin = 0;
	unsigned int rate_iops = 0;
	unsigned int rate_iops_min = 0;

	assert(ddir_rw(ddir));

	if (!td->o.ratemin[ddir] && !td->o.rate_iops_min[ddir])
		return 0;

	/*
	 * allow a 2 second settle period in the beginning
	 */
	if (mtime_since(&td->start, now) < 2000)
		return 0;

	iops += td->this_io_blocks[ddir];
	bytes += td->this_io_bytes[ddir];
	ratemin += td->o.ratemin[ddir];
	rate_iops += td->o.rate_iops[ddir];
	rate_iops_min += td->o.rate_iops_min[ddir];

	/*
	 * if rate blocks is set, sample is running
	 */
	if (td->rate_bytes[ddir] || td->rate_blocks[ddir]) {
		spent = mtime_since(&td->lastrate[ddir], now);
		if (spent < td->o.ratecycle)
			return 0;

		if (td->o.rate[ddir]) {
			/*
			 * check bandwidth specified rate
			 */
			if (bytes < td->rate_bytes[ddir]) {
				log_err("%s: min rate %u not met\n", td->o.name,
								ratemin);
				return 1;
			} else {
				rate = ((bytes - td->rate_bytes[ddir]) * 1000) / spent;
				if (rate < ratemin ||
				    bytes < td->rate_bytes[ddir]) {
					log_err("%s: min rate %u not met, got"
						" %luKB/sec\n", td->o.name,
							ratemin, rate);
					return 1;
				}
			}
		} else {
			/*
			 * checks iops specified rate
			 */
			if (iops < rate_iops) {
				log_err("%s: min iops rate %u not met\n",
						td->o.name, rate_iops);
				return 1;
			} else {
				rate = ((iops - td->rate_blocks[ddir]) * 1000) / spent;
				if (rate < rate_iops_min ||
				    iops < td->rate_blocks[ddir]) {
					log_err("%s: min iops rate %u not met,"
						" got %lu\n", td->o.name,
							rate_iops_min, rate);
				}
			}
		}
	}

	td->rate_bytes[ddir] = bytes;
	td->rate_blocks[ddir] = iops;
	memcpy(&td->lastrate[ddir], now, sizeof(*now));
	return 0;
}

static int check_min_rate(struct thread_data *td, struct timeval *now,
			  unsigned long *bytes_done)
{
	int ret = 0;

	if (bytes_done[0])
		ret |= __check_min_rate(td, now, 0);
	if (bytes_done[1])
		ret |= __check_min_rate(td, now, 1);

	return ret;
}

static inline int runtime_exceeded(struct thread_data *td, struct timeval *t)
{
	if (in_ramp_time(td))
		return 0;
	if (!td->o.timeout)
		return 0;
	if (mtime_since(&td->epoch, t) >= td->o.timeout * 1000)
		return 1;

	return 0;
}

/*
 * When job exits, we can cancel the in-flight IO if we are using async
 * io. Attempt to do so.
 */
static void cleanup_pending_aio(struct thread_data *td)
{
	struct flist_head *entry, *n;
	struct io_u *io_u;
	int r;

	/*
	 * get immediately available events, if any
	 */
	r = io_u_queued_complete(td, 0, NULL);
	if (r < 0)
		return;

	/*
	 * now cancel remaining active events
	 */
	if (td->io_ops->cancel) {
		flist_for_each_safe(entry, n, &td->io_u_busylist) {
			io_u = flist_entry(entry, struct io_u, list);

			/*
			 * if the io_u isn't in flight, then that generally
			 * means someone leaked an io_u. complain but fix
			 * it up, so we don't stall here.
			 */
			if ((io_u->flags & IO_U_F_FLIGHT) == 0) {
				log_err("fio: non-busy IO on busy list\n");
				put_io_u(td, io_u);
			} else {
				r = td->io_ops->cancel(td, io_u);
				if (!r)
					put_io_u(td, io_u);
			}
		}
	}

	if (td->cur_depth)
		r = io_u_queued_complete(td, td->cur_depth, NULL);
}

/*
 * Helper to handle the final sync of a file. Works just like the normal
 * io path, just does everything sync.
 */
static int fio_io_sync(struct thread_data *td, struct fio_file *f)
{
	struct io_u *io_u = __get_io_u(td);
	int ret;

	if (!io_u)
		return 1;

	io_u->ddir = DDIR_SYNC;
	io_u->file = f;

	if (td_io_prep(td, io_u)) {
		put_io_u(td, io_u);
		return 1;
	}

requeue:
	ret = td_io_queue(td, io_u);
	if (ret < 0) {
		td_verror(td, io_u->error, "td_io_queue");
		put_io_u(td, io_u);
		return 1;
	} else if (ret == FIO_Q_QUEUED) {
		if (io_u_queued_complete(td, 1, NULL) < 0)
			return 1;
	} else if (ret == FIO_Q_COMPLETED) {
		if (io_u->error) {
			td_verror(td, io_u->error, "td_io_queue");
			return 1;
		}

		if (io_u_sync_complete(td, io_u, NULL) < 0)
			return 1;
	} else if (ret == FIO_Q_BUSY) {
		if (td_io_commit(td))
			return 1;
		goto requeue;
	}

	return 0;
}

static inline void __update_tv_cache(struct thread_data *td)
{
	fio_gettime(&td->tv_cache, NULL);
}

static inline void update_tv_cache(struct thread_data *td)
{
	if ((++td->tv_cache_nr & td->tv_cache_mask) == td->tv_cache_mask)
		__update_tv_cache(td);
}

static int break_on_this_error(struct thread_data *td, enum fio_ddir ddir,
			       int *retptr)
{
	int ret = *retptr;

	if (ret < 0 || td->error) {
		int err;

		if (ret < 0)
			err = -ret;
		else
			err = td->error;

		if (!(td->o.continue_on_error & td_error_type(ddir, err)))
			return 1;

		if (td_non_fatal_error(err)) {
		        /*
		         * Continue with the I/Os in case of
			 * a non fatal error.
			 */
			update_error_count(td, err);
			td_clear_error(td);
			*retptr = 0;
			return 0;
		} else if (td->o.fill_device && err == ENOSPC) {
			/*
			 * We expect to hit this error if
			 * fill_device option is set.
			 */
			td_clear_error(td);
			td->terminate = 1;
			return 1;
		} else {
			/*
			 * Stop the I/O in case of a fatal
			 * error.
			 */
			update_error_count(td, err);
			return 1;
		}
	}

	return 0;
}

/*
 * The main verify engine. Runs over the writes we previously submitted,
 * reads the blocks back in, and checks the crc/md5 of the data.
 */
static void do_verify(struct thread_data *td)
{
	struct fio_file *f;
	struct io_u *io_u;
	int ret, min_events;
	unsigned int i;

	dprint(FD_VERIFY, "starting loop\n");

	/*
	 * sync io first and invalidate cache, to make sure we really
	 * read from disk.
	 */
	for_each_file(td, f, i) {
		if (!fio_file_open(f))
			continue;
		if (fio_io_sync(td, f))
			break;
		if (file_invalidate_cache(td, f))
			break;
	}

	if (td->error)
		return;

	td_set_runstate(td, TD_VERIFYING);

	io_u = NULL;
	while (!td->terminate) {
		int ret2, full;

		update_tv_cache(td);

		if (runtime_exceeded(td, &td->tv_cache)) {
			__update_tv_cache(td);
			if (runtime_exceeded(td, &td->tv_cache)) {
				td->terminate = 1;
				break;
			}
		}

		io_u = __get_io_u(td);
		if (!io_u)
			break;

		if (get_next_verify(td, io_u)) {
			put_io_u(td, io_u);
			break;
		}

		if (td_io_prep(td, io_u)) {
			put_io_u(td, io_u);
			break;
		}

		if (td->o.verify_async)
			io_u->end_io = verify_io_u_async;
		else
			io_u->end_io = verify_io_u;

		ret = td_io_queue(td, io_u);
		switch (ret) {
		case FIO_Q_COMPLETED:
			if (io_u->error) {
				ret = -io_u->error;
				clear_io_u(td, io_u);
			} else if (io_u->resid) {
				int bytes = io_u->xfer_buflen - io_u->resid;

				/*
				 * zero read, fail
				 */
				if (!bytes) {
					td_verror(td, EIO, "full resid");
					put_io_u(td, io_u);
					break;
				}

				io_u->xfer_buflen = io_u->resid;
				io_u->xfer_buf += bytes;
				io_u->offset += bytes;

				if (ddir_rw(io_u->ddir))
					td->ts.short_io_u[io_u->ddir]++;

				f = io_u->file;
				if (io_u->offset == f->real_file_size)
					goto sync_done;

				requeue_io_u(td, &io_u);
			} else {
sync_done:
				ret = io_u_sync_complete(td, io_u, NULL);
				if (ret < 0)
					break;
			}
			continue;
		case FIO_Q_QUEUED:
			break;
		case FIO_Q_BUSY:
			requeue_io_u(td, &io_u);
			ret2 = td_io_commit(td);
			if (ret2 < 0)
				ret = ret2;
			break;
		default:
			assert(ret < 0);
			td_verror(td, -ret, "td_io_queue");
			break;
		}

		if (break_on_this_error(td, io_u->ddir, &ret))
			break;

		/*
		 * if we can queue more, do so. but check if there are
		 * completed io_u's first. Note that we can get BUSY even
		 * without IO queued, if the system is resource starved.
		 */
		full = queue_full(td) || (ret == FIO_Q_BUSY && td->cur_depth);
		if (full || !td->o.iodepth_batch_complete) {
			min_events = min(td->o.iodepth_batch_complete,
					 td->cur_depth);
			if (full && !min_events && td->o.iodepth_batch_complete != 0)
				min_events = 1;

			do {
				/*
				 * Reap required number of io units, if any,
				 * and do the verification on them through
				 * the callback handler
				 */
				if (io_u_queued_complete(td, min_events, NULL) < 0) {
					ret = -1;
					break;
				}
			} while (full && (td->cur_depth > td->o.iodepth_low));
		}
		if (ret < 0)
			break;
	}

	if (!td->error) {
		min_events = td->cur_depth;

		if (min_events)
			ret = io_u_queued_complete(td, min_events, NULL);
	} else
		cleanup_pending_aio(td);

	td_set_runstate(td, TD_RUNNING);

	dprint(FD_VERIFY, "exiting loop\n");
}

/*
 * Main IO worker function. It retrieves io_u's to process and queues
 * and reaps them, checking for rate and errors along the way.
 */
static void do_io(struct thread_data *td)
{
	unsigned int i;
	int ret = 0;

	if (in_ramp_time(td))
		td_set_runstate(td, TD_RAMP);
	else
		td_set_runstate(td, TD_RUNNING);

	while ( (td->o.read_iolog_file && !flist_empty(&td->io_log_list)) ||
		(!flist_empty(&td->trim_list)) ||
	        ((td->this_io_bytes[0] + td->this_io_bytes[1]) < td->o.size) ) {
		struct timeval comp_time;
		unsigned long bytes_done[2] = { 0, 0 };
		int min_evts = 0;
		struct io_u *io_u;
		int ret2, full;
		enum fio_ddir ddir;

		if (td->terminate)
			break;

		update_tv_cache(td);

		if (runtime_exceeded(td, &td->tv_cache)) {
			__update_tv_cache(td);
			if (runtime_exceeded(td, &td->tv_cache)) {
				td->terminate = 1;
				break;
			}
		}

		io_u = get_io_u(td);
		if (!io_u)
			break;

		ddir = io_u->ddir;

		/*
		 * Add verification end_io handler, if asked to verify
		 * a previously written file.
		 */
		if (td->o.verify != VERIFY_NONE && io_u->ddir == DDIR_READ &&
		    !td_rw(td)) {
			if (td->o.verify_async)
				io_u->end_io = verify_io_u_async;
			else
				io_u->end_io = verify_io_u;
			td_set_runstate(td, TD_VERIFYING);
		} else if (in_ramp_time(td))
			td_set_runstate(td, TD_RAMP);
		else
			td_set_runstate(td, TD_RUNNING);

		ret = td_io_queue(td, io_u);
		switch (ret) {
		case FIO_Q_COMPLETED:
			if (io_u->error) {
				ret = -io_u->error;
				clear_io_u(td, io_u);
			} else if (io_u->resid) {
				int bytes = io_u->xfer_buflen - io_u->resid;
				struct fio_file *f = io_u->file;

				/*
				 * zero read, fail
				 */
				if (!bytes) {
					td_verror(td, EIO, "full resid");
					put_io_u(td, io_u);
					break;
				}

				io_u->xfer_buflen = io_u->resid;
				io_u->xfer_buf += bytes;
				io_u->offset += bytes;

				if (ddir_rw(io_u->ddir))
					td->ts.short_io_u[io_u->ddir]++;

				if (io_u->offset == f->real_file_size)
					goto sync_done;

				requeue_io_u(td, &io_u);
			} else {
sync_done:
				if (__should_check_rate(td, 0) ||
				    __should_check_rate(td, 1))
					fio_gettime(&comp_time, NULL);

				ret = io_u_sync_complete(td, io_u, bytes_done);
				if (ret < 0)
					break;
			}
			break;
		case FIO_Q_QUEUED:
			/*
			 * if the engine doesn't have a commit hook,
			 * the io_u is really queued. if it does have such
			 * a hook, it has to call io_u_queued() itself.
			 */
			if (td->io_ops->commit == NULL)
				io_u_queued(td, io_u);
			break;
		case FIO_Q_BUSY:
			requeue_io_u(td, &io_u);
			ret2 = td_io_commit(td);
			if (ret2 < 0)
				ret = ret2;
			break;
		default:
			assert(ret < 0);
			put_io_u(td, io_u);
			break;
		}

		if (break_on_this_error(td, ddir, &ret))
			break;

		/*
		 * See if we need to complete some commands. Note that we
		 * can get BUSY even without IO queued, if the system is
		 * resource starved.
		 */
		full = queue_full(td) || (ret == FIO_Q_BUSY && td->cur_depth);
		if (full || !td->o.iodepth_batch_complete) {
			min_evts = min(td->o.iodepth_batch_complete,
					td->cur_depth);
			if (full && !min_evts && td->o.iodepth_batch_complete != 0)
				min_evts = 1;

			if (__should_check_rate(td, 0) ||
			    __should_check_rate(td, 1))
				fio_gettime(&comp_time, NULL);

			do {
				ret = io_u_queued_complete(td, min_evts, bytes_done);
				if (ret < 0)
					break;

			} while (full && (td->cur_depth > td->o.iodepth_low));
		}

		if (ret < 0)
			break;
		if (!(bytes_done[0] + bytes_done[1]))
			continue;

		if (!in_ramp_time(td) && should_check_rate(td, bytes_done)) {
			if (check_min_rate(td, &comp_time, bytes_done)) {
				if (exitall_on_terminate)
					fio_terminate_threads(td->groupid);
				td_verror(td, EIO, "check_min_rate");
				break;
			}
		}

		if (td->o.thinktime) {
			unsigned long long b;

			b = td->io_blocks[0] + td->io_blocks[1];
			if (!(b % td->o.thinktime_blocks)) {
				int left;

				if (td->o.thinktime_spin)
					usec_spin(td->o.thinktime_spin);

				left = td->o.thinktime - td->o.thinktime_spin;
				if (left)
					usec_sleep(td, left);
			}
		}
	}

	if (td->trim_entries)
		log_err("fio: %d trim entries leaked?\n", td->trim_entries);

	if (td->o.fill_device && td->error == ENOSPC) {
		td->error = 0;
		td->terminate = 1;
	}
	if (!td->error) {
		struct fio_file *f;

		i = td->cur_depth;
		if (i) {
			ret = io_u_queued_complete(td, i, NULL);
			if (td->o.fill_device && td->error == ENOSPC)
				td->error = 0;
		}

		if (should_fsync(td) && td->o.end_fsync) {
			td_set_runstate(td, TD_FSYNCING);

			for_each_file(td, f, i) {
				if (!fio_file_open(f))
					continue;
				fio_io_sync(td, f);
			}
		}
	} else
		cleanup_pending_aio(td);

	/*
	 * stop job if we failed doing any IO
	 */
	if ((td->this_io_bytes[0] + td->this_io_bytes[1]) == 0)
		td->done = 1;
}

static void cleanup_io_u(struct thread_data *td)
{
	struct flist_head *entry, *n;
	struct io_u *io_u;

	flist_for_each_safe(entry, n, &td->io_u_freelist) {
		io_u = flist_entry(entry, struct io_u, list);

		flist_del(&io_u->list);
		fio_memfree(io_u, sizeof(*io_u));
	}

	free_io_mem(td);
}

static int init_io_u(struct thread_data *td)
{
	struct io_u *io_u;
	unsigned int max_bs;
	int cl_align, i, max_units;
	char *p;

	max_units = td->o.iodepth;
	max_bs = max(td->o.max_bs[DDIR_READ], td->o.max_bs[DDIR_WRITE]);
	td->orig_buffer_size = (unsigned long long) max_bs
					* (unsigned long long) max_units;

	if (td->o.mem_type == MEM_SHMHUGE || td->o.mem_type == MEM_MMAPHUGE) {
		unsigned long bs;

		bs = td->orig_buffer_size + td->o.hugepage_size - 1;
		td->orig_buffer_size = bs & ~(td->o.hugepage_size - 1);
	}

	if (td->orig_buffer_size != (size_t) td->orig_buffer_size) {
		log_err("fio: IO memory too large. Reduce max_bs or iodepth\n");
		return 1;
	}

	if (allocate_io_mem(td))
		return 1;

	if (td->o.odirect || td->o.mem_align ||
	    (td->io_ops->flags & FIO_RAWIO))
		p = PAGE_ALIGN(td->orig_buffer) + td->o.mem_align;
	else
		p = td->orig_buffer;

	cl_align = os_cache_line_size();

	for (i = 0; i < max_units; i++) {
		void *ptr;

		if (td->terminate)
			return 1;

		ptr = fio_memalign(cl_align, sizeof(*io_u));
		if (!ptr) {
			log_err("fio: unable to allocate aligned memory\n");
			break;
		}

		io_u = ptr;
		memset(io_u, 0, sizeof(*io_u));
		INIT_FLIST_HEAD(&io_u->list);
		dprint(FD_MEM, "io_u alloc %p, index %u\n", io_u, i);

		if (!(td->io_ops->flags & FIO_NOIO)) {
			io_u->buf = p;
			dprint(FD_MEM, "io_u %p, mem %p\n", io_u, io_u->buf);

			if (td_write(td))
				io_u_fill_buffer(td, io_u, max_bs);
			if (td_write(td) && td->o.verify_pattern_bytes) {
				/*
				 * Fill the buffer with the pattern if we are
				 * going to be doing writes.
				 */
				fill_pattern(td, io_u->buf, max_bs, io_u, 0, 0);
			}
		}

		io_u->index = i;
		io_u->flags = IO_U_F_FREE;
		flist_add(&io_u->list, &td->io_u_freelist);
		p += max_bs;
	}

	return 0;
}

static int switch_ioscheduler(struct thread_data *td)
{
	char tmp[256], tmp2[128];
	FILE *f;
	int ret;

	if (td->io_ops->flags & FIO_DISKLESSIO)
		return 0;

	sprintf(tmp, "%s/queue/scheduler", td->sysfs_root);

	f = fopen(tmp, "r+");
	if (!f) {
		if (errno == ENOENT) {
			log_err("fio: os or kernel doesn't support IO scheduler"
				" switching\n");
			return 0;
		}
		td_verror(td, errno, "fopen iosched");
		return 1;
	}

	/*
	 * Set io scheduler.
	 */
	ret = fwrite(td->o.ioscheduler, strlen(td->o.ioscheduler), 1, f);
	if (ferror(f) || ret != 1) {
		td_verror(td, errno, "fwrite");
		fclose(f);
		return 1;
	}

	rewind(f);

	/*
	 * Read back and check that the selected scheduler is now the default.
	 */
	ret = fread(tmp, 1, sizeof(tmp), f);
	if (ferror(f) || ret < 0) {
		td_verror(td, errno, "fread");
		fclose(f);
		return 1;
	}

	sprintf(tmp2, "[%s]", td->o.ioscheduler);
	if (!strstr(tmp, tmp2)) {
		log_err("fio: io scheduler %s not found\n", td->o.ioscheduler);
		td_verror(td, EINVAL, "iosched_switch");
		fclose(f);
		return 1;
	}

	fclose(f);
	return 0;
}

static int keep_running(struct thread_data *td)
{
	unsigned long long io_done;

	if (td->done)
		return 0;
	if (td->o.time_based)
		return 1;
	if (td->o.loops) {
		td->o.loops--;
		return 1;
	}

	io_done = td->io_bytes[DDIR_READ] + td->io_bytes[DDIR_WRITE]
			+ td->io_skip_bytes;
	if (io_done < td->o.size)
		return 1;

	return 0;
}

static void reset_io_counters(struct thread_data *td)
{
	td->stat_io_bytes[0] = td->stat_io_bytes[1] = 0;
	td->this_io_bytes[0] = td->this_io_bytes[1] = 0;
	td->stat_io_blocks[0] = td->stat_io_blocks[1] = 0;
	td->this_io_blocks[0] = td->this_io_blocks[1] = 0;
	td->zone_bytes = 0;
	td->rate_bytes[0] = td->rate_bytes[1] = 0;
	td->rate_blocks[0] = td->rate_blocks[1] = 0;

	td->last_was_sync = 0;

	/*
	 * reset file done count if we are to start over
	 */
	if (td->o.time_based || td->o.loops)
		td->nr_done_files = 0;
}

void reset_all_stats(struct thread_data *td)
{
	struct timeval tv;
	int i;

	reset_io_counters(td);

	for (i = 0; i < 2; i++) {
		td->io_bytes[i] = 0;
		td->io_blocks[i] = 0;
		td->io_issues[i] = 0;
		td->ts.total_io_u[i] = 0;
	}

	fio_gettime(&tv, NULL);
	td->ts.runtime[0] = 0;
	td->ts.runtime[1] = 0;
	memcpy(&td->epoch, &tv, sizeof(tv));
	memcpy(&td->start, &tv, sizeof(tv));
}

static void clear_io_state(struct thread_data *td)
{
	struct fio_file *f;
	unsigned int i;

	reset_io_counters(td);

	close_files(td);
	for_each_file(td, f, i)
		fio_file_clear_done(f);

	/*
	 * Set the same seed to get repeatable runs
	 */
	td_fill_rand_seeds(td);
}

static int exec_string(const char *string)
{
	int ret, newlen = strlen(string) + 1 + 8;
	char *str;

	str = malloc(newlen);
	sprintf(str, "sh -c %s", string);

	ret = system(str);
	if (ret == -1)
		log_err("fio: exec of cmd <%s> failed\n", str);

	free(str);
	return ret;
}

/*
 * Entry point for the thread based jobs. The process based jobs end up
 * here as well, after a little setup.
 */
static void *thread_main(void *data)
{
	unsigned long long elapsed;
	struct thread_data *td = data;
	pthread_condattr_t attr;
	int clear_state;

	if (!td->o.use_thread) {
		setsid();
		td->pid = getpid();
	} else
		td->pid = gettid();

	dprint(FD_PROCESS, "jobs pid=%d started\n", (int) td->pid);

	INIT_FLIST_HEAD(&td->io_u_freelist);
	INIT_FLIST_HEAD(&td->io_u_busylist);
	INIT_FLIST_HEAD(&td->io_u_requeues);
	INIT_FLIST_HEAD(&td->io_log_list);
	INIT_FLIST_HEAD(&td->io_hist_list);
	INIT_FLIST_HEAD(&td->verify_list);
	INIT_FLIST_HEAD(&td->trim_list);
	pthread_mutex_init(&td->io_u_lock, NULL);
	td->io_hist_tree = RB_ROOT;

	pthread_condattr_init(&attr);
	pthread_cond_init(&td->verify_cond, &attr);
	pthread_cond_init(&td->free_cond, &attr);

	td_set_runstate(td, TD_INITIALIZED);
	dprint(FD_MUTEX, "up startup_mutex\n");
	fio_mutex_up(startup_mutex);
	dprint(FD_MUTEX, "wait on td->mutex\n");
	fio_mutex_down(td->mutex);
	dprint(FD_MUTEX, "done waiting on td->mutex\n");

	/*
	 * the ->mutex mutex is now no longer used, close it to avoid
	 * eating a file descriptor
	 */
	fio_mutex_remove(td->mutex);

	/*
	 * A new gid requires privilege, so we need to do this before setting
	 * the uid.
	 */
	if (td->o.gid != -1U && setgid(td->o.gid)) {
		td_verror(td, errno, "setgid");
		goto err;
	}
	if (td->o.uid != -1U && setuid(td->o.uid)) {
		td_verror(td, errno, "setuid");
		goto err;
	}

	/*
	 * If we have a gettimeofday() thread, make sure we exclude that
	 * thread from this job
	 */
	if (td->o.gtod_cpu)
		fio_cpu_clear(&td->o.cpumask, td->o.gtod_cpu);

	/*
	 * Set affinity first, in case it has an impact on the memory
	 * allocations.
	 */
	if (td->o.cpumask_set && fio_setaffinity(td->pid, td->o.cpumask) == -1) {
		td_verror(td, errno, "cpu_set_affinity");
		goto err;
	}

	/*
	 * May alter parameters that init_io_u() will use, so we need to
	 * do this first.
	 */
	if (init_iolog(td))
		goto err;

	if (init_io_u(td))
		goto err;

	if (td->o.verify_async && verify_async_init(td))
		goto err;

	if (td->ioprio_set) {
		if (ioprio_set(IOPRIO_WHO_PROCESS, 0, td->ioprio) == -1) {
			td_verror(td, errno, "ioprio_set");
			goto err;
		}
	}

	if (td->o.cgroup_weight && cgroup_setup(td, cgroup_list, &cgroup_mnt))
		goto err;

	if (nice(td->o.nice) == -1) {
		td_verror(td, errno, "nice");
		goto err;
	}

	if (td->o.ioscheduler && switch_ioscheduler(td))
		goto err;

	if (!td->o.create_serialize && setup_files(td))
		goto err;

	if (td_io_init(td))
		goto err;

	if (init_random_map(td))
		goto err;

	if (td->o.exec_prerun) {
		if (exec_string(td->o.exec_prerun))
			goto err;
	}

	if (td->o.pre_read) {
		if (pre_read_files(td) < 0)
			goto err;
	}

	fio_gettime(&td->epoch, NULL);
	getrusage(RUSAGE_SELF, &td->ru_start);

	clear_state = 0;
	while (keep_running(td)) {
		fio_gettime(&td->start, NULL);
		memcpy(&td->bw_sample_time, &td->start, sizeof(td->start));
		memcpy(&td->iops_sample_time, &td->start, sizeof(td->start));
		memcpy(&td->tv_cache, &td->start, sizeof(td->start));

		if (td->o.ratemin[0] || td->o.ratemin[1]) {
		        memcpy(&td->lastrate[0], &td->bw_sample_time,
						sizeof(td->bw_sample_time));
		        memcpy(&td->lastrate[1], &td->bw_sample_time,
						sizeof(td->bw_sample_time));
		}

		if (clear_state)
			clear_io_state(td);

		prune_io_piece_log(td);

		do_io(td);

		clear_state = 1;

		if (td_read(td) && td->io_bytes[DDIR_READ]) {
			elapsed = utime_since_now(&td->start);
			td->ts.runtime[DDIR_READ] += elapsed;
		}
		if (td_write(td) && td->io_bytes[DDIR_WRITE]) {
			elapsed = utime_since_now(&td->start);
			td->ts.runtime[DDIR_WRITE] += elapsed;
		}

		if (td->error || td->terminate)
			break;

		if (!td->o.do_verify ||
		    td->o.verify == VERIFY_NONE ||
		    (td->io_ops->flags & FIO_UNIDIR))
			continue;

		clear_io_state(td);

		fio_gettime(&td->start, NULL);

		do_verify(td);

		td->ts.runtime[DDIR_READ] += utime_since_now(&td->start);

		if (td->error || td->terminate)
			break;
	}

	update_rusage_stat(td);
	td->ts.runtime[0] = (td->ts.runtime[0] + 999) / 1000;
	td->ts.runtime[1] = (td->ts.runtime[1] + 999) / 1000;
	td->ts.total_run_time = mtime_since_now(&td->epoch);
	td->ts.io_bytes[0] = td->io_bytes[0];
	td->ts.io_bytes[1] = td->io_bytes[1];

	fio_mutex_down(writeout_mutex);
	if (td->bw_log) {
		if (td->o.bw_log_file) {
			finish_log_named(td, td->bw_log,
						td->o.bw_log_file, "bw");
		} else
			finish_log(td, td->bw_log, "bw");
	}
	if (td->lat_log) {
		if (td->o.lat_log_file) {
			finish_log_named(td, td->lat_log,
						td->o.lat_log_file, "lat");
		} else
			finish_log(td, td->lat_log, "lat");
	}
	if (td->slat_log) {
		if (td->o.lat_log_file) {
			finish_log_named(td, td->slat_log,
						td->o.lat_log_file, "slat");
		} else
			finish_log(td, td->slat_log, "slat");
	}
	if (td->clat_log) {
		if (td->o.lat_log_file) {
			finish_log_named(td, td->clat_log,
						td->o.lat_log_file, "clat");
		} else
			finish_log(td, td->clat_log, "clat");
	}
	if (td->iops_log) {
		if (td->o.iops_log_file) {
			finish_log_named(td, td->iops_log,
						td->o.iops_log_file, "iops");
		} else
			finish_log(td, td->iops_log, "iops");
	}

	fio_mutex_up(writeout_mutex);
	if (td->o.exec_postrun)
		exec_string(td->o.exec_postrun);

	if (exitall_on_terminate)
		fio_terminate_threads(td->groupid);

err:
	if (td->error)
		log_info("fio: pid=%d, err=%d/%s\n", (int) td->pid, td->error,
							td->verror);

	if (td->o.verify_async)
		verify_async_exit(td);

	close_and_free_files(td);
	close_ioengine(td);
	cleanup_io_u(td);
	cgroup_shutdown(td, &cgroup_mnt);

	if (td->o.cpumask_set) {
		int ret = fio_cpuset_exit(&td->o.cpumask);

		td_verror(td, ret, "fio_cpuset_exit");
	}

	/*
	 * do this very late, it will log file closing as well
	 */
	if (td->o.write_iolog_file)
		write_iolog_close(td);

	td_set_runstate(td, TD_EXITED);
	return (void *) (unsigned long) td->error;
}

/*
 * We cannot pass the td data into a forked process, so attach the td and
 * pass it to the thread worker.
 */
static int fork_main(int shmid, int offset)
{
	struct thread_data *td;
	void *data, *ret;

#ifndef __hpux
	data = shmat(shmid, NULL, 0);
	if (data == (void *) -1) {
		int __err = errno;

		perror("shmat");
		return __err;
	}
#else
	/*
	 * HP-UX inherits shm mappings?
	 */
	data = threads;
#endif

	td = data + offset * sizeof(struct thread_data);
	ret = thread_main(td);
	shmdt(data);
	return (int) (unsigned long) ret;
}

/*
 * Run over the job map and reap the threads that have exited, if any.
 */
static void reap_threads(unsigned int *nr_running, unsigned int *t_rate,
			 unsigned int *m_rate)
{
	struct thread_data *td;
	unsigned int cputhreads, realthreads, pending;
	int i, status, ret;

	/*
	 * reap exited threads (TD_EXITED -> TD_REAPED)
	 */
	realthreads = pending = cputhreads = 0;
	for_each_td(td, i) {
		int flags = 0;

		/*
		 * ->io_ops is NULL for a thread that has closed its
		 * io engine
		 */
		if (td->io_ops && !strcmp(td->io_ops->name, "cpuio"))
			cputhreads++;
		else
			realthreads++;

		if (!td->pid) {
			pending++;
			continue;
		}
		if (td->runstate == TD_REAPED)
			continue;
		if (td->o.use_thread) {
			if (td->runstate == TD_EXITED) {
				td_set_runstate(td, TD_REAPED);
				goto reaped;
			}
			continue;
		}

		flags = WNOHANG;
		if (td->runstate == TD_EXITED)
			flags = 0;

		/*
		 * check if someone quit or got killed in an unusual way
		 */
		ret = waitpid(td->pid, &status, flags);
		if (ret < 0) {
			if (errno == ECHILD) {
				log_err("fio: pid=%d disappeared %d\n",
						(int) td->pid, td->runstate);
				td_set_runstate(td, TD_REAPED);
				goto reaped;
			}
			perror("waitpid");
		} else if (ret == td->pid) {
			if (WIFSIGNALED(status)) {
				int sig = WTERMSIG(status);

				if (sig != SIGTERM)
					log_err("fio: pid=%d, got signal=%d\n",
							(int) td->pid, sig);
				td_set_runstate(td, TD_REAPED);
				goto reaped;
			}
			if (WIFEXITED(status)) {
				if (WEXITSTATUS(status) && !td->error)
					td->error = WEXITSTATUS(status);

				td_set_runstate(td, TD_REAPED);
				goto reaped;
			}
		}

		/*
		 * thread is not dead, continue
		 */
		pending++;
		continue;
reaped:
		(*nr_running)--;
		(*m_rate) -= (td->o.ratemin[0] + td->o.ratemin[1]);
		(*t_rate) -= (td->o.rate[0] + td->o.rate[1]);
		if (!td->pid)
			pending--;

		if (td->error)
			exit_value++;

		done_secs += mtime_since_now(&td->epoch) / 1000;
	}

	if (*nr_running == cputhreads && !pending && realthreads)
		fio_terminate_threads(TERMINATE_ALL);
}

static void *gtod_thread_main(void *data)
{
	fio_mutex_up(startup_mutex);

	/*
	 * As long as we have jobs around, update the clock. It would be nice
	 * to have some way of NOT hammering that CPU with gettimeofday(),
	 * but I'm not sure what to use outside of a simple CPU nop to relax
	 * it - we don't want to lose precision.
	 */
	while (threads) {
		fio_gtod_update();
		nop;
	}

	return NULL;
}

static int fio_start_gtod_thread(void)
{
	pthread_attr_t attr;
	int ret;

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN);
	ret = pthread_create(&gtod_thread, &attr, gtod_thread_main, NULL);
	pthread_attr_destroy(&attr);
	if (ret) {
		log_err("Can't create gtod thread: %s\n", strerror(ret));
		return 1;
	}

	ret = pthread_detach(gtod_thread);
	if (ret) {
		log_err("Can't detatch gtod thread: %s\n", strerror(ret));
		return 1;
	}

	dprint(FD_MUTEX, "wait on startup_mutex\n");
	fio_mutex_down(startup_mutex);
	dprint(FD_MUTEX, "done waiting on startup_mutex\n");
	return 0;
}

/*
 * Main function for kicking off and reaping jobs, as needed.
 */
static void run_threads(void)
{
	struct thread_data *td;
	unsigned long spent;
	unsigned int i, todo, nr_running, m_rate, t_rate, nr_started;

	if (fio_pin_memory())
		return;

	if (fio_gtod_offload && fio_start_gtod_thread())
		return;

	set_sig_handlers();

	if (!terse_output) {
		log_info("Starting ");
		if (nr_thread)
			log_info("%d thread%s", nr_thread,
						nr_thread > 1 ? "s" : "");
		if (nr_process) {
			if (nr_thread)
				log_info(" and ");
			log_info("%d process%s", nr_process,
						nr_process > 1 ? "es" : "");
		}
		log_info("\n");
		fflush(stdout);
	}

	todo = thread_number;
	nr_running = 0;
	nr_started = 0;
	m_rate = t_rate = 0;

	for_each_td(td, i) {
		print_status_init(td->thread_number - 1);

		if (!td->o.create_serialize)
			continue;

		/*
		 * do file setup here so it happens sequentially,
		 * we don't want X number of threads getting their
		 * client data interspersed on disk
		 */
		if (setup_files(td)) {
			exit_value++;
			if (td->error)
				log_err("fio: pid=%d, err=%d/%s\n",
					(int) td->pid, td->error, td->verror);
			td_set_runstate(td, TD_REAPED);
			todo--;
		} else {
			struct fio_file *f;
			unsigned int j;

			/*
			 * for sharing to work, each job must always open
			 * its own files. so close them, if we opened them
			 * for creation
			 */
			for_each_file(td, f, j) {
				if (fio_file_open(f))
					td_io_close_file(td, f);
			}
		}
	}

	set_genesis_time();

	while (todo) {
		struct thread_data *map[REAL_MAX_JOBS];
		struct timeval this_start;
		int this_jobs = 0, left;

		/*
		 * create threads (TD_NOT_CREATED -> TD_CREATED)
		 */
		for_each_td(td, i) {
			if (td->runstate != TD_NOT_CREATED)
				continue;

			/*
			 * never got a chance to start, killed by other
			 * thread for some reason
			 */
			if (td->terminate) {
				todo--;
				continue;
			}

			if (td->o.start_delay) {
				spent = mtime_since_genesis();

				if (td->o.start_delay * 1000 > spent)
					continue;
			}

			if (td->o.stonewall && (nr_started || nr_running)) {
				dprint(FD_PROCESS, "%s: stonewall wait\n",
							td->o.name);
				break;
			}

			init_disk_util(td);

			/*
			 * Set state to created. Thread will transition
			 * to TD_INITIALIZED when it's done setting up.
			 */
			td_set_runstate(td, TD_CREATED);
			map[this_jobs++] = td;
			nr_started++;

			if (td->o.use_thread) {
				int ret;

				dprint(FD_PROCESS, "will pthread_create\n");
				ret = pthread_create(&td->thread, NULL,
							thread_main, td);
				if (ret) {
					log_err("pthread_create: %s\n",
							strerror(ret));
					nr_started--;
					break;
				}
				ret = pthread_detach(td->thread);
				if (ret)
					log_err("pthread_detach: %s",
							strerror(ret));
			} else {
				pid_t pid;
				dprint(FD_PROCESS, "will fork\n");
				pid = fork();
				if (!pid) {
					int ret = fork_main(shm_id, i);

					_exit(ret);
				} else if (i == fio_debug_jobno)
					*fio_debug_jobp = pid;
			}
			dprint(FD_MUTEX, "wait on startup_mutex\n");
			if (fio_mutex_down_timeout(startup_mutex, 10)) {
				log_err("fio: job startup hung? exiting.\n");
				fio_terminate_threads(TERMINATE_ALL);
				fio_abort = 1;
				nr_started--;
				break;
			}
			dprint(FD_MUTEX, "done waiting on startup_mutex\n");
		}

		/*
		 * Wait for the started threads to transition to
		 * TD_INITIALIZED.
		 */
		fio_gettime(&this_start, NULL);
		left = this_jobs;
		while (left && !fio_abort) {
			if (mtime_since_now(&this_start) > JOB_START_TIMEOUT)
				break;

			usleep(100000);

			for (i = 0; i < this_jobs; i++) {
				td = map[i];
				if (!td)
					continue;
				if (td->runstate == TD_INITIALIZED) {
					map[i] = NULL;
					left--;
				} else if (td->runstate >= TD_EXITED) {
					map[i] = NULL;
					left--;
					todo--;
					nr_running++; /* work-around... */
				}
			}
		}

		if (left) {
			log_err("fio: %d jobs failed to start\n", left);
			for (i = 0; i < this_jobs; i++) {
				td = map[i];
				if (!td)
					continue;
				kill(td->pid, SIGTERM);
			}
			break;
		}

		/*
		 * start created threads (TD_INITIALIZED -> TD_RUNNING).
		 */
		for_each_td(td, i) {
			if (td->runstate != TD_INITIALIZED)
				continue;

			if (in_ramp_time(td))
				td_set_runstate(td, TD_RAMP);
			else
				td_set_runstate(td, TD_RUNNING);
			nr_running++;
			nr_started--;
			m_rate += td->o.ratemin[0] + td->o.ratemin[1];
			t_rate += td->o.rate[0] + td->o.rate[1];
			todo--;
			fio_mutex_up(td->mutex);
		}

		reap_threads(&nr_running, &t_rate, &m_rate);

		if (todo) {
			if (is_backend)
				fio_server_idle_loop();
			else
				usleep(100000);
		}
	}

	while (nr_running) {
		reap_threads(&nr_running, &t_rate, &m_rate);

		if (is_backend)
			fio_server_idle_loop();
		else
			usleep(10000);
	}

	update_io_ticks();
	fio_unpin_memory();
}

int exec_run(void)
{
	struct thread_data *td;
	int i;

	if (nr_clients)
		return fio_handle_clients();
	if (exec_profile) {
		if (load_profile(exec_profile))
			return 1;
		free(exec_profile);
		exec_profile = NULL;
	}
	if (!thread_number)
		return 0;

	if (write_bw_log) {
		setup_log(&agg_io_log[DDIR_READ], 0);
		setup_log(&agg_io_log[DDIR_WRITE], 0);
	}

	startup_mutex = fio_mutex_init(0);
	if (startup_mutex == NULL)
		return 1;
	writeout_mutex = fio_mutex_init(1);
	if (writeout_mutex == NULL)
		return 1;

	set_genesis_time();
	create_disk_util_thread();

	cgroup_list = smalloc(sizeof(*cgroup_list));
	INIT_FLIST_HEAD(cgroup_list);

	run_threads();

	if (!fio_abort) {
		show_run_stats();
		if (write_bw_log) {
			__finish_log(agg_io_log[DDIR_READ], "agg-read_bw.log");
			__finish_log(agg_io_log[DDIR_WRITE],
					"agg-write_bw.log");
		}
	}

	for_each_td(td, i)
		fio_options_free(td);

	cgroup_kill(cgroup_list);
	sfree(cgroup_list);
	sfree(cgroup_mnt);

	fio_mutex_remove(startup_mutex);
	fio_mutex_remove(writeout_mutex);
	return exit_value;
}

void reset_fio_state(void)
{
	groupid = 0;
	thread_number = 0;
	nr_process = 0;
	nr_thread = 0;
	done_secs = 0;
}

static int endian_check(void)
{
	union {
		uint8_t c[8];
		uint64_t v;
	} u;
	int le = 0, be = 0;

	u.v = 0x12;
	if (u.c[7] == 0x12)
		be = 1;
	else if (u.c[0] == 0x12)
		le = 1;

#if defined(FIO_LITTLE_ENDIAN)
	if (be)
		return 1;
#elif defined(FIO_BIG_ENDIAN)
	if (le)
		return 1;
#else
	return 1;
#endif

	if (!le && !be)
		return 1;

	return 0;
}

int main(int argc, char *argv[], char *envp[])
{
	long ps;

	if (endian_check()) {
		log_err("fio: endianness settings appear wrong.\n");
		log_err("fio: please report this to fio@vger.kernel.org\n");
		return 1;
	}

	arch_init(envp);

	sinit();

	/*
	 * We need locale for number printing, if it isn't set then just
	 * go with the US format.
	 */
	if (!getenv("LC_NUMERIC"))
		setlocale(LC_NUMERIC, "en_US");

	ps = sysconf(_SC_PAGESIZE);
	if (ps < 0) {
		log_err("Failed to get page size\n");
		return 1;
	}

	page_size = ps;
	page_mask = ps - 1;

	fio_keywords_init();

	if (parse_options(argc, argv))
		return 1;

	return exec_run();
}
