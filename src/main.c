/*
 * ctsup: A tiny illumos contract supervisor.
 *
 * Usage:
 *
 *     ctsup /path/to/services
 *
 * Every regular executable file directly inside the directory is treated
 * as a service:
 *
 *     services/
 *         dnsmasq
 *         update-hosts-list
 *
 * Each service:
 *
 *   - is executed directly (no shell)
 *   - receives its own illumos process contract
 *   - is restarted RESTART_DELAY second after its contract becomes empty
 *
 * On SIGTERM or SIGINT:
 *
 *   - stop restarting services
 *   - send SIGTERM to every service contract
 *   - wait up to the graceful shutdown timeout
 *   - SIGKILL anything still alive
 *   - wait for all contracts to become empty
 *   - abandon the contracts and exit
 *
 */

/*
 * # Created
 * Author: Dave Eddy <ysap@daveeddy.com>
 * Date: August 29, 2026
 * License: MIT
 *
 * # Contributors
 * - Dave Eddy <ysap@daveeddy.com>
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/contract.h>
#include <sys/contract/process.h>
#include <sys/ctfs.h>
#include <sys/procset.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <libcontract.h>

#define PROGNAME "ctsup"

// seconds to sleep before restarting a service
#define RESTART_DELAY 1

// when instructed to shutdown, milliseconds between asking services to shutdown
// nicely with SIGTERM and forcing services to shutdown with SIGKILL
#define SHUTDOWN_TIMEOUT_MS 5000

// when instructed to shutdown, how often to resend the SIGTERM/SIGKILL signals
// to the services (until everything is killed)
#define SIGNAL_REPEAT_MS 250

// maximum time to wait for a contract event before checking for shutdown
#define EVENT_POLL_MS 250

// timestamp format used for logging
#define DATEFMT "%Y-%m-%dT%H:%M:%S"

/*
 * A linked-list of all "service" objects found.
 *
 * Every regular executable file found in the services directory is treated as a
 * service.
 */
typedef struct service {
	char		 name[NAME_MAX + 1];
	char		 path[PATH_MAX];
	ctid_t		 ctid;
	struct service	*next;
} service_t;

/*
 * Command line flags and environment options.
 */
static struct opts {
	bool color;
	bool quiet;
} g_opts;

static service_t *g_services;
static volatile sig_atomic_t g_shutting_down;

/*
 * Open an internal file descriptor with close-on-exec, retrying if the call
 * is interrupted by a signal.
 *
 * When opening a file in CTFS we can't pass O_CLOEXEC because we will get hit
 * with EINVAL.  Instead, we open the file normally and *then* set the
 * FD_CLOEXEC flag after the fact which works for both regular files and files
 * inside in CTFS.
 *
 * Since we only have a single-thread in this program we don't have a
 * race-condition to worry about.
 */
static int
open_internal(const char *path, int flags)
{
	int fd;

	// open the file
	do {
		fd = open(path, flags);
	} while (fd == -1 && errno == EINTR);
	if (fd == -1) {
		return -1;
	}

	// get the flags set on the fd
	int fdflags;
	do {
		fdflags = fcntl(fd, F_GETFD);
	} while (fdflags == -1 && errno == EINTR);
	if (fdflags == -1) {
		int err = errno;
		close(fd);
		errno = err;
		return -1;
	}

	// add CLOEXEC to the fd
	int res;
	do {
		res = fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC);
	} while (res == -1 && errno == EINTR);
	if (res == -1) {
		int err = errno;
		close(fd);
		errno = err;
		return -1;
	}

	return fd;
}

/*
 * Print the usage message.
 */
static void
usage(FILE *s)
{
	fprintf(s, "Usage: %s <dir>\n", PROGNAME);
	fprintf(s, "\n");
	fprintf(s, "Environment Variables\n");
	fprintf(s, "  CTSUP_QUIET       disable log messages from this program\n");
	fprintf(s, "  CTSUP_NO_COLOR    disables color output from this program\n");
	fprintf(s, "  NO_COLOR          same as above but may affect downstream services\n");
}

/*
 * Print a timestamped log line.
 *
 * Usage is the same as printf().
 */
static void LOG(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void
LOG(const char *fmt, ...)
{
	if (g_opts.quiet) {
		return;
	}

	if (g_opts.color) {
		printf("\x1b[2m");
	}

	// print timestamp
	struct timeval tv;
	int res = gettimeofday(&tv, NULL);
	if (res == 0) {
		char date[64];
		strftime(date, sizeof(date) / sizeof(date[0]), DATEFMT,
		    gmtime(&tv.tv_sec));
		printf("[%s.%03ldZ] ", date, tv.tv_usec / 1000);
	} else {
		// todo: something else?
		printf("[<unknown time>] ");
	}

	// print the rest of the args
	va_list args;
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);

	if (g_opts.color) {
		printf("\x1b[0m");
	}

	fflush(stdout);
}

/*
 * Signal handler.
 *
 * Intentionally kept simple so the main work can be processed in the main
 * thread not in an interrupt context.
 */
static void
handle_signal(int signal)
{
	(void) signal;
	g_shutting_down = 1;
}

/*
 * Set up signal handling.
 *
 * Returns 0 on success, -1 on error.
 */
static int
setup_signals(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof (sa));
	sigemptyset(&sa.sa_mask);

	sa.sa_handler = handle_signal;
	sa.sa_flags = 0;

	if (sigaction(SIGTERM, &sa, NULL) == -1) {
		return -1;
	}

	if (sigaction(SIGINT, &sa, NULL) == -1) {
		return -1;
	}

	sa.sa_handler = SIG_IGN;
	sa.sa_flags = SA_NOCLDWAIT;

	if (sigaction(SIGCHLD, &sa, NULL) == -1) {
		return -1;
	}

	return 0;
}

/*
 * Find the process contract created by our most recent fork().
 *
 * Returns the id on success, -1 on error.
 */
static ctid_t
latest_contract(void)
{
	int fd = open_internal(CTFS_ROOT "/process/latest", O_RDONLY);
	if (fd == -1) {
		return -1;
	}

	ct_stathdl_t st;
	int err = ct_status_read(fd, CTD_COMMON, &st);
	close(fd);

	if (err != 0) {
		errno = err;
		return -1;
	}

	ctid_t id = ct_status_get_id(st);
	ct_status_free(st);

	return id;
}

/*
 * Create and activate a process-contract template.
 *
 * Returns the fd number on success, -1 on error.
 */
static int
contract_template(void)
{
	int err;

	int fd = open_internal(CTFS_ROOT "/process/template", O_RDWR);
	if (fd == -1) {
		return -1;
	}

	err = ct_tmpl_set_critical(fd, CT_PR_EV_EMPTY);
	if (err != 0) {
		goto fail;
	}

	err = ct_tmpl_set_informative(fd, 0);
	if (err != 0) {
		goto fail;
	}

	err = ct_pr_tmpl_set_fatal(fd, CT_PR_EV_HWERR);
	if (err != 0) {
		goto fail;
	}

	err = ct_pr_tmpl_set_param(fd, CT_PR_NOORPHAN);
	if (err != 0) {
		goto fail;
	}

	err = ct_tmpl_activate(fd);
	if (err != 0) {
		goto fail;
	}

	return fd;

fail:
	close(fd);
	errno = err;
	return -1;
}

/*
 * Start one service in a new process contract
 *
 * Returns 0 on success, -1 on error.
 */
static int
start_service(service_t *svc)
{
	int tmpl = contract_template();
	if (tmpl == -1) {
		perror("contract template");
		return -1;
	}

	// block shutdown signals so the child doesn't react to them
	sigset_t blockset;
	sigset_t oldset;
	sigemptyset(&blockset);
	sigaddset(&blockset, SIGTERM);
	sigaddset(&blockset, SIGINT);
	if (sigprocmask(SIG_BLOCK, &blockset, &oldset) == -1) {
		int err = errno;
		ct_tmpl_clear(tmpl);
		close(tmpl);
		errno = err;
		perror("sigprocmask");
		return -1;
	}

	// fork the process and immediately close the contract template - we
	// store the errno so we can print it in the event of a fork(2) problem.
	pid_t pid = fork();
	int forkerr = errno;
	int clearerr = ct_tmpl_clear(tmpl);

	close(tmpl);

	// fork broke - we can't meaningfully continue
	if (pid == -1) {
		sigprocmask(SIG_SETMASK, &oldset, NULL);
		errno = forkerr;
		perror("fork");
		return -1;
	}

	if (pid == 0) {
		// we are the child
		if (clearerr != 0) {
			errno = clearerr;
			perror("ct_tmpl_clear");
			_exit(127); // TODO: is 127 appropriate here?
		}

		// in the child let signals fall through normally
		struct sigaction sa;
		memset(&sa, 0, sizeof (sa));
		sa.sa_handler = SIG_DFL;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0;
		if (sigaction(SIGTERM, &sa, NULL) == -1 ||
		    sigaction(SIGINT, &sa, NULL) == -1 ||
		    sigaction(SIGCHLD, &sa, NULL) == -1) {
			perror("sigaction");
			_exit(127);
		}
		if (sigprocmask(SIG_SETMASK, &oldset, NULL) == -1) {
			perror("sigprocmask");
			_exit(127);
		}

		// start the service
		execl(svc->path, svc->name, (char *)NULL);
		perror(svc->path);
		_exit(127);
	}

	// unblock signals
	int maskerr = 0;
	if (sigprocmask(SIG_SETMASK, &oldset, NULL) == -1) {
		maskerr = errno;
	}

	// get the process of the last forked program
	ctid_t ctid = latest_contract();
	if (ctid == -1) {
		LOG("%s: cannot determine contract: %s\n",
		    svc->name, strerror(errno));

		// we can't supervise the process if we can't get its contract
		// id
		kill(pid, SIGKILL);

		return -1;
	}

	svc->ctid = ctid;
	LOG("%s: started pid %ld contract %ld\n", svc->name, (long)pid,
	    (long)ctid);

	if (clearerr != 0) {
		errno = clearerr;
		LOG("%s: cannot clear contract template: %s\n",
		    svc->name, strerror(errno));
		return -1;
	}

	if (maskerr != 0) {
		errno = maskerr;
		LOG("%s: cannot restore signal mask: %s\n",
		    svc->name, strerror(errno));
		return -1;
	}

	return 0;
}

/*
 * Find the service belonging to a contract.
 *
 * Returns a pointer to the service, or NULL.
 */
static service_t *
find_service(ctid_t ctid)
{
	service_t *svc;

	for (svc = g_services; svc != NULL; svc = svc->next) {
		if (svc->ctid == ctid) {
			return svc;
		}
	}

	return NULL;
}

/*
 * Abandon a contract we no longer need.
 *
 * We only use this after the contract has become empty.
 *
 * Returns 0 on success, -1 on error.
 */
static int
abandon_contract(ctid_t ctid)
{
	char path[PATH_MAX];
	snprintf(path, sizeof (path), CTFS_ROOT "/process/%ld/ctl",
	    (long)ctid);

	int fd = open_internal(path, O_WRONLY);
	if (fd == -1) {
		return -1;
	}

	int err = ct_ctl_abandon(fd);
	close(fd);

	if (err != 0) {
		errno = err;
		return -1;
	}

	return 0;
}

/*
 * Send a signal to every process currently belonging to a contract.
 */
static int
signal_contract(ctid_t ctid, int sig)
{
	return sigsend(P_CTID, ctid, sig);
}

/*
 * Send a signal to every currently-running service.
 *
 * Returns 0 on success and -1 if any contract could not be signaled.  ESRCH
 * is ignored because the contract may already be empty with its event pending.
 */
static int
signal_services(int sig)
{
	int saved_errno = 0;
	service_t *svc;

	for (svc = g_services; svc != NULL; svc = svc->next) {
		if (svc->ctid == -1) {
			continue;
		}

		if (signal_contract(svc->ctid, sig) == -1) {
			if (errno == ESRCH) {
				continue;
			}

			int err = errno;
			LOG("%s: cannot signal contract %ld: %s\n",
			    svc->name, (long)svc->ctid, strerror(err));
			if (saved_errno == 0) {
				saved_errno = err;
			}
		}
	}

	if (saved_errno != 0) {
		errno = saved_errno;
		return -1;
	}

	return 0;
}

/*
 * Check if all services have been killed / are stopped.
 */
static bool
all_services_stopped(void)
{
	service_t *svc;

	for (svc = g_services; svc != NULL; svc = svc->next) {
		if (svc->ctid != -1) {
			return false;
		}
	}

	return true;
}

/*
 * Process one event from the process-contract bundle.
 *
 * Returns:
 *
 *     1   handled an EMPTY event for one of our services
 *     0   event was irrelevant
 *    -1   error
 */
static int
process_contract_event(int eventfd, service_t **emptied)
{
	*emptied = NULL;

	// read the event
	ct_evthdl_t ev;
	int err = ct_event_read(eventfd, &ev);
	if (err != 0) {
		if (err == EINTR) {
			return 0;
		}

		errno = err;
		return -1;
	}

	// figure out event type
	ctid_t ctid = ct_event_get_ctid(ev);
	uint_t type = ct_event_get_type(ev);
	if (type != CT_PR_EV_EMPTY) {
		ct_event_free(ev);
		return 0;
	}

	// if we are here then one of our contracts is now empty! meaning, it
	// no longer has PIDs in it.

	// find the associated "service" object for the contract id
	service_t *svc = find_service(ctid);
	if (svc == NULL) {
		ct_event_free(ev);
		return 0;
	}

	LOG("%s: contract %ld empty\n", svc->name, (long)ctid);

	/*
	 * The EMPTY event means the service is stopped even if abandoning its
	 * contract fails.  Clear the id first so shutdown does not wait for an
	 * EMPTY event that has already been consumed.
	 */
	svc->ctid = -1;

	// abandon the contract... we'll make a new one when we restart it
	if (abandon_contract(ctid) == -1) {
		ct_event_free(ev);
		return -1;
	}

	*emptied = svc;
	ct_event_free(ev);

	return 1;
}

/*
 * Scan the service directory for services
 *
 * Returns the number of services created, or -1 on error.
 */
static int
load_services(const char *dir)
{
	DIR *dp = opendir(dir);
	if (dp == NULL) {
		perror(dir);
		return -1;
	}

	// loop the directories
	int n = 0;
	while (true) {
		// clear errno to test for readdir errors
		errno = 0;
		struct dirent *de = readdir(dp);
		if (de == NULL) {
			int err = errno;

			if (err == 0) {
				// readdir finished successfully
				break;
			} else {
				// readdir had an error
				closedir(dp);
				fprintf(stderr, "readdir %s: %s\n", dir,
				    strerror(err));
				errno = err;
				return -1;
			}
		}

		// skip hidden files
		if (de->d_name[0] == '.') {
			continue;
		}

		// get full path to file
		char path[PATH_MAX];
		int len = snprintf(path, sizeof (path), "%s/%s", dir,
		    de->d_name);
		if (len < 0 || (size_t)len >= sizeof (path)) {
			LOG("path too long: %s/%s\n", dir, de->d_name);
			continue;
		}

		// stat the file (allow symlinks)
		struct stat st;
		if (stat(path, &st) == -1) {
			LOG("stat %s: %s\n", path, strerror(errno));
			continue;
		}

		// skip anything but regular files
		if (!S_ISREG(st.st_mode)) {
			continue;
		}

		// skip files that aren't executable
		if (access(path, X_OK) == -1) {
			continue;
		}

		// the service is valid! hang onto it
		service_t *svc = calloc(1, sizeof (*svc));
		if (svc == NULL) {
			perror("calloc");
			closedir(dp);
			return -1;
		}

		strlcpy(svc->name, de->d_name, sizeof (svc->name));
		strlcpy(svc->path, path, sizeof (svc->path));
		svc->ctid = -1;
		svc->next = g_services;

		g_services = svc;

		n++;
	}

	closedir(dp);
	return n;
}

/*
 * Poll for contract activity for up to timeout_ms.
 *
 * Returns:
 *
 *     1   an event is available
 *     0   timeout or signal interruption
 *    -1   error
 */
static int
wait_for_event(int eventfd, int timeout_ms)
{
	struct pollfd pfd;
	pfd.fd = eventfd;
	pfd.events = POLLIN;
	pfd.revents = 0;

	int res = poll(&pfd, 1, timeout_ms);

	if (res == -1) {
		if (errno == EINTR) {
			return 0;
		}
		return -1;
	}

	if (res == 0) {
		return 0;
	}

	if (pfd.revents & POLLIN) {
		return 1;
	}

	// a positive poll result without POLLIN is an endpoint failure or an
	// event this function cannot handle - just give up.
	if (pfd.revents & POLLNVAL) {
		errno = EBADF;
	} else {
		errno = EIO;
	}

	return -1;
}

/*
 * Signal active contracts and consume EMPTY events until all services have
 * stopped.  A deadline of zero means wait indefinitely.
 *
 * Returns 1 when all services have stopped, 0 at the deadline, and -1 on
 * error.
 */
static int
drain_services(int eventfd, int sig, hrtime_t deadline)
{
	hrtime_t next_signal = 0;

	/*
	 * keep trying until everything is dead or we hit the optional timeout.
	 * the signal is sent immediately, and then periodically after to make
	 * sure that any new processes that have been forked will know they need
	 * to die.
	 */
	while (!all_services_stopped()) {
		service_t *empty;
		hrtime_t now = gethrtime();

		// check deadline timer
		if (deadline != 0 && now >= deadline) {
			return 0;
		}

		// only signal every SIGNAL_REPEAT_MS
		if (now >= next_signal) {
			if (signal_services(sig) == -1) {
				return -1;
			}
			next_signal = now +
			    (hrtime_t)SIGNAL_REPEAT_MS * 1000000;
		}

		// wait until the next signal is due, unless the overall
		// shutdown deadline comes first.
		hrtime_t wait_until = next_signal;
		if (deadline != 0 && deadline < wait_until) {
			wait_until = deadline;
		}

		hrtime_t remaining = wait_until - gethrtime();
		int timeout = remaining <= 0 ? 0 :
		    (int)((remaining + 999999) / 1000000);

		// wait for the next event or signal interval
		int res = wait_for_event(eventfd, timeout);
		if (res == -1) {
			return -1;
		}

		if (res == 1 && process_contract_event(eventfd, &empty) == -1) {
			return -1;
		}
	}

	return 1;
}

/*
 * Gracefully shut down everything we're supervising.
 *
 * Returns 0 on success and -1 on error.
 */
static int
shutdown_services(int eventfd)
{
	hrtime_t deadline = gethrtime() +
	    (hrtime_t)SHUTDOWN_TIMEOUT_MS * 1000000;

	LOG("sending SIGTERM to all contracts\n");
	int res = drain_services(eventfd, SIGTERM, deadline);
	if (res == -1) {
		return -1;
	} else if (res == 1) {
		return 0;
	}

	LOG("shutdown timeout reached - killing remaining services\n");

	LOG("sending SIGKILL to all contracts\n");
	return drain_services(eventfd, SIGKILL, 0) == -1 ? -1 : 0;
}

int
main(int argc, char **argv)
{
	int exit_status = 0;
	g_opts.color = getenv("NO_COLOR") == NULL &&
	               getenv("CTSUP_NO_COLOR") == NULL;
	g_opts.quiet = getenv("CTSUP_QUIET") != NULL;

	if (argc < 2) {
		usage(stderr);
		return 2;
	}

	char *service_dir = argv[1];
	if (strcmp(service_dir, "-h") == 0 ||
	    strcmp(service_dir, "--help") == 0) {
		usage(stdout);
		return 0;
	}

	int n = load_services(service_dir);
	if (n == -1) {
		// the function above handles printing the error message
		return 1;
	}

	// ensure we found at least 1 service
	if (n == 0) {
		fprintf(stderr, "no executable services in %s\n", service_dir);
		return 1;
	}

	LOG("found %d service files\n", n);

	if (setup_signals() == -1) {
		perror("setup signals");
		return 1;
	}

	int eventfd = open_internal(CTFS_ROOT "/process/pbundle", O_RDONLY);
	if (eventfd == -1) {
		perror("open pbundle");
		return 1;
	}

	// start every service
	service_t *svc;
	for (svc = g_services; svc != NULL; svc = svc->next) {
		if (start_service(svc) == -1) {
			exit_status = 1;
			g_shutting_down = 1;
			break;
		}
	}

	// main loop - monitor all running services
	while (!g_shutting_down) {
		int res = wait_for_event(eventfd, EVENT_POLL_MS);
		if (res == -1) {
			// poll failed - we have to just exit
			perror("wait_for_event");
			exit_status = 1;
			break;
		} else if (res == 0) {
			// try again
			continue;
		}

		// process the event for the contract
		service_t *empty;
		res = process_contract_event(eventfd, &empty);
		if (res == -1) {
			perror("process_contract_event");
			exit_status = 1;
			break;
		}

		// the event was irrelevant - try again
		if (empty == NULL) {
			continue;
		}

		/*
		 * if we are here then a contract is empty and has been
		 * abandoned - that means we are about to sleep and then restart
		 * it.  to be overly cautious let's check before and after we
		 * sleep if we were asked to shutdown via a signal.
		 */
		if (g_shutting_down) {
			break;
		}

		LOG("%s: restarting in %d second\n",
		    empty->name, RESTART_DELAY);
		sleep(RESTART_DELAY);

		if (g_shutting_down) {
			break;
		}

		// (re)start the service
		if (start_service(empty) == -1) {
			exit_status = 1;
			break;
		}
	}

	LOG("shutting down.\n");
	if (shutdown_services(eventfd) == -1) {
		perror("shutdown");
		exit_status = 1;
	}
	close(eventfd);

	return exit_status;
}
