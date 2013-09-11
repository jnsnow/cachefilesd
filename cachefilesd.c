/* CacheFiles userspace management daemon
 *
 * Copyright (C) 2006-2007 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 *
 * Configuration file goes in /etc/cachefiles.conf and is of the form:
 *
 *	dir /var/cache/fscache
 *	tag mycache
 *	brun 10%
 *	bcull 7%
 *	bstop 3%
 *	frun 10%
 *	fcull 7%
 *	fstop 3%
 *
 * Only "dir" is mandatory
 * Blank lines and lines beginning with a hash are comments
 * Trailing spaces are significant
 * There is no character escaping mechanism
 * NUL characters are cause for error
 */

#define CACHEFILESD_VERSION "0.10.6"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <syslog.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <syslog.h>
#include <dirent.h>
#include <time.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/time.h>
#include <sys/vfs.h>
#include <sys/stat.h>

#include "common/fsck.h"
#include "common/debug.h"
#include "common/cull.h"

typedef enum objtype {
	OBJTYPE_INDEX,
	OBJTYPE_DATA,
	OBJTYPE_SPECIAL,
	OBJTYPE_INTERMEDIATE,
} objtype_t;

DIR *rootdir = NULL;
static int nopendir = 0;

/* ranked order of cullable objects */
/* 2^12 = 4096 entries */
static unsigned culltable_exponent = 12;
static unsigned culltable_size = 4096;
static struct queue *cullq;

static const char *configfile = "/etc/cachefilesd.conf";
static const char *devfile = "/dev/cachefiles";
static const char *procfile = "/proc/fs/cachefiles";
static const char *pidfile = "/var/run/cachefilesd.pid";
static char *cacheroot, *graveyardpath;

int stop;
static int reap, cull, nocull;
static int graveyardfd;
static int jumpstart_scan = 1;
static int refresh = 0;
static int refresh_rate = 30;
static unsigned long long brun, bcull, bstop, frun, fcull, fstop;

/* cachefilesd currently supports a single cache. */
static struct cachefilesd_state *state = NULL;

/* Maximum number of retries when we fail to cull anything */
static const int thrash_limit = 5;

static __attribute__((noreturn))
void version(void)
{
	printf("cachefilesd version " CACHEFILESD_VERSION "\n");
	exit(0);
}

static __attribute__((noreturn))
void help(void)
{
	fprintf(stderr,
		"Format:\n"
		"  /sbin/cachefilesd [-d]* [-s] [-n] [-p <pidfile>] "
		"[-f <configfile>]\n"
		"  /sbin/cachefilesd -v\n"
		"\n"
		"Options:\n"
		"  -d\tIncrease debugging level (cumulative)\n"
		"  -n\tDon't daemonise the process\n"
		"  -s\tMessage output to stderr instead of syslog\n"
		"  -p <pidfile>\tWrite the PID into the file\n"
		"  -f <configfile>\n"
		"  -v\tPrint version and exit\n"
		"  -c\tCheck cache consistency and exit\n"
		"  -F\tForce a deep-scan.\n"
		"\tRead the specified configuration file instead of"
		" /etc/cachefiles.conf\n");

	exit(2);
}

static void open_cache(void);
static void cachefilesd(void) __attribute__((noreturn));
static void reap_graveyard(void);
static void reap_graveyard_aux(const char *dirname);
static void read_cache_state(void);

/*****************************************************************************/
/*
 * termination request
 */
static void sigterm(int sig)
{
	stop = 1;
}

/*****************************************************************************/
/*
 * the graveyard was populated
 */
static void sigio(int sig)
{
	reap = 1;
}

/*****************************************************************************/
/*
 * signal to refresh the queue
 */
static void sigalrm(int sig)
{
	refresh = 1;
}

/*****************************************************************************/
/*
 * write the PID file
 */
static void write_pidfile(void)
{
	FILE *pf;

	pf = fopen(pidfile, "w");
	if (!pf)
		oserror("Unable to open PID file: %s", pidfile);

	if (fprintf(pf, "%d\n", getpid()) < 0 ||
	    fclose(pf) == EOF)
		oserror("Unable to write PID file: %s", pidfile);
}


void read_config(const char *configfile, char offline)
{
	unsigned lineno;
	size_t m;
	char *line, *cp;
	ssize_t n;
	struct stat st;
	long page_size;
	FILE *fh;

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size < 0)
		oserror("Unable to get page size");

	fh = fopen(configfile, "r");
	if (!fh)
		oserror("Unable to open %s", configfile);

	/* read the configuration */
	m = 0;
	line = NULL;
	lineno = 0;

	while (n = getline(&line, &m, fh), n != EOF) {
		lineno++;

		if (n >= page_size)
			cfgerror("Line too long");

		if (memchr(line, 0, n) != 0)
			cfgerror("Line contains a NUL character");

		/* eat blank lines, leading white space and trailing NL */
		cp = strchr(line, '\n');
		if (!cp)
			cfgerror("Unterminated line");

		if (cp == line)
			continue;
		*cp = '\0';

		for (cp = line; isspace(*cp); cp++) {;}

		if (!*cp)
			continue;

		/* eat full line comments */
		if (*cp == '#')
			continue;

		/*  allow culling to be disabled */
		if (memcmp(cp, "nocull", 6) == 0 &&
		    (!cp[6] || isspace(cp[6]))) {
			nocull = 1;
		}

		/* note the cull table size command */
		if (memcmp(cp, "culltable", 9) == 0 && isspace(cp[9])) {
			unsigned long cts;
			char *sp;

			for (sp = cp + 10; isspace(*sp); sp++) {;}

			cts = strtoul(sp, &sp, 10);
			if (*sp)
				cfgerror("Invalid cull table size number");
			if (cts < 12 || cts > 20)
				cfgerror("Log2 of cull table size"
					 " must be 12 <= N <= 20");
			culltable_size = 1 << cts;
			culltable_exponent = cts;
			continue;
		}

		/* note the dir command */
		if (memcmp(cp, "dir", 3) == 0 && isspace(cp[3])) {
			char *sp;

			for (sp = cp + 4; isspace(*sp); sp++) {;}

			if (strlen(sp) > PATH_MAX - 10)
				cfgerror("Cache pathname is too long");

			if (stat(sp, &st) < 0)
				oserror("Can't confirm cache location");

			cacheroot = strdup(sp);
			if (!cacheroot)
				oserror("Can't copy cache name");
		}

		/* object to the bind command */
		if (memcmp(cp, "bind", 4) == 0 &&
		    (!cp[4] || isspace(cp[4])))
			cfgerror("'bind' command not permitted");

		/* pass the config options over to the kernel module */
		if (!offline) {
			if (write(cachefd, line, strlen(line)) < 0) {
				if (errno == -ENOMEM || errno == -EIO)
					oserror("CacheFiles");
				cfgerror("CacheFiles gave config error: %m");
			}
		}
	}

	if (line)
		free(line);

	if (!feof(fh))
		oserror("Unable to read %s", configfile);

	if (fclose(fh) == EOF)
		oserror("Unable to close %s", configfile);

}

/**
 * To appease valgrind, primarily.
 */
void cachefilesd_cleanup(void)
{
	if (rootdir) {
		closedir(rootdir);
		rootdir = NULL;
	}

	if (state) state_destroy(&state);

	free(graveyardpath);
	free(cacheroot);

	if (cullq) {
		delete_queue(cullq);
		cullq = NULL;
	}

	graveyardpath = NULL;
	cacheroot = NULL;
}

/*****************************************************************************/
/*
 * start up the cache and go
 */
int main(int argc, char *argv[])
{
	int _cachefd, nullfd, opt, loop, open_max, rc, nodaemon = 0;
	int force_scan = 0;
	int scan_only = 0;

	/* handle help request */
	if (argc == 2 && strcmp(argv[1], "--help") == 0)
		help();

	if (argc == 2 && strcmp(argv[1], "--version") == 0)
		version();

	/* parse the arguments */
	while (opt = getopt(argc, argv, "dsnf:p:vFc"),
	       opt != EOF) {
		switch (opt) {
		case 'd':
			/* turn on debugging */
			xdebug++;
			break;

		case 's':
			/* disable syslog writing */
			xnolog = 1;
			break;

		case 'n':
			/* don't daemonise */
			nodaemon = 1;
			break;

		case 'f':
			/* use a specific config file */
			configfile = optarg;
			break;

		case 'p':
			/* use a specific PID file */
			pidfile = optarg;
			break;

		case 'c':
			/* offline scanning mode.
			 * Do not engage the kernel. */
			scan_only = 1;
			break;

		case 'F':
			/* Force a deep scan */
			force_scan = 1;
			break;

		case 'v':
			/* print the version and exit */
			version();

		default:
			opterror("Unknown commandline option '%c'", optopt);
		}
	}

	/* read various parameters */
	open_max = sysconf(_SC_OPEN_MAX);
	if (open_max < 0)
		oserror("Unable to get max open files");

	/* become owned by root */
	if (setresuid(0, 0, 0) < 0)
		oserror("Unable to set UID to 0");

	if (setresgid(0, 0, 0) < 0)
		oserror("Unable to set GID to 0");

	/* just in case... */
	sync();

	if (scan_only) {
		/* If we're only doing a scan, do not try
		 * to engage with the kernel module. */
		goto rconfig;
	}

	/* open the devfile or the procfile on fd 3 */
	_cachefd = open(devfile, O_RDWR);
	if (_cachefd < 0) {
		if (errno != ENOENT)
			oserror("Unable to open %s", devfile);

		_cachefd = open(procfile, O_RDWR);
		if (_cachefd < 0) {
			if (errno == ENOENT)
				oserror("Unable to open %s", devfile);
			oserror("Unable to open %s", procfile);
		}
	}

	if (_cachefd != cachefd) {
		if (dup2(_cachefd, cachefd) < 0)
			oserror("Unable to transfer cache fd to 3");
		if (close(_cachefd) < 0)
			oserror("Close of original cache fd failed");
	}

rconfig:
	/* Read the configuration file. */
	read_config(configfile, scan_only);

	/* open /dev/null */
	nullfd = open("/dev/null", O_RDWR);
	if (nullfd < 0)
		oserror("Unable to open /dev/null");

	/* leave stdin, stdout, stderr and cachefd open only */
	if (nullfd != 0)
		dup2(nullfd, 0);
	if (nullfd != 1)
		dup2(nullfd, 1);

	for (loop = 4; loop < open_max; loop++)
		close(loop);

	/* set up a connection to syslog whilst we still can (the bind command
	 * will give us our own namespace with no /dev/log */
	openlog("cachefilesd", LOG_PID, LOG_DAEMON);
	xopenedlog = 1;

	rc = state_init(&state, cacheroot);
	if (rc) {
		debug(0, "Error initializing cache state.");
		return rc;
	}
	state->need_fsck = force_scan;

	/* perform a quick scan. If problems are identified,
	 * need_fsck will be set to true, and we can handle
	 * this with a deep scan after we bind the cache.
	 */
	if ((rc = cachefilesd_fsck_light(cacheroot, &state))) {
		debug(0, "Error during preliminary sanity check.");
		return rc;
	}

	/* We intend to scan-only, then jump ship. */
	if (scan_only) {
		if (state->need_fsck) {
			rc = cachefilesd_fsck_deep(state);
			if (rc)
				debug(0, "Encountered issues during deep scan.");
			else
				info("cull_index fsck completed successfully.");
		}
		cachefilesd_cleanup();
		return rc;
	}


	/* Now that the scan is completed and we have state,
	 * create a culling queue associated with this state. */
	if (!nocull) {
		cullq = new_queue(culltable_exponent, state);
	}


	info("About to bind cache");

	/* now issue the bind command */
	if (write(cachefd, "bind", 4) < 0)
		oserror("CacheFiles bind failed");
	state->bound = 1;
	info("Bound cache");

	/* we now have a live cache - daemonise the process */
	if (!nodaemon) {
		if (!xdebug)
			dup2(1, 2);

		switch (fork()) {
		case -1:
			oserror("fork");

		case 0:
			if (xdebug)
				fprintf(stderr, "Daemon PID %d\n", getpid());

			signal(SIGTTIN, SIG_IGN);
			signal(SIGTTOU, SIG_IGN);
			signal(SIGTSTP, SIG_IGN);
			setsid();
			write_pidfile();
			cachefilesd();

		default:
			break;
		}
	}
	else {
		cachefilesd();
	}

	exit(0);
}

/*****************************************************************************/
/*
 * open the cache directories
 */
static void open_cache(void)
{
	struct statfs sfs;
	char buffer[PATH_MAX + 1];

	/* open the cache directory so we can scan it */
	snprintf(buffer, PATH_MAX, "%s/cache", cacheroot);

	debug(1, "open_cache(%s)\n", buffer);
	rootdir = opendir(buffer);
	if (!rootdir)
		oserror("Unable to open cache directory");
	nopendir++;

	/* open the graveyard so we can set a notification on it */
	if (asprintf(&graveyardpath, "%s/graveyard", cacheroot) < 0)
		oserror("Unable to copy graveyard name");

	graveyardfd = open(graveyardpath, O_DIRECTORY);
	if (graveyardfd < 0)
		oserror("Unable to open graveyard directory");

	if (fstatfs(graveyardfd, &sfs) < 0)
		oserror("Unable to stat cache filesystem");

	if (sfs.f_bsize == -1 ||
	    sfs.f_blocks == -1 ||
	    sfs.f_bfree == -1 ||
	    sfs.f_bavail == -1)
		error("Backing filesystem returns unusable statistics through fstatfs()");
}

/*****************************************************************************/
/*
 * manage the cache
 */
static void cachefilesd(void)
{
	struct timeval tv;
	/* wolfram alpha tells me that an 8 byte unsigned value in usecs
	 * can span about 584.6 average gregorian millennia,
	 * so we probably don't have to worry about overflow. */
	size_t usecs;
	sigset_t sigs, osigs;
	int rc;

	struct pollfd pollfds[1] = {
		[0] = {
			.fd	= cachefd,
			.events	= POLLIN,
		},
	};

	notice("Daemon Started");

	/* open the cache directories */
	open_cache();

	/* we need to disable I/O and termination signals so they're only
	 * caught at appropriate times
	 */
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGIO);
	sigaddset(&sigs, SIGINT);
	sigaddset(&sigs, SIGTERM);

	signal(SIGTERM, sigterm);
	signal(SIGINT, sigterm);

	/* check the graveyard for graves */
	reap_graveyard();

	while (!stop) {
		read_cache_state();

		/* sleep without racing on reap and cull with the signal
		 * handlers */
		if (!jumpstart_scan && !reap && !cull &&
		    !(state->need_fsck && !state->fsck_running)) {
			if (sigprocmask(SIG_BLOCK, &sigs, &osigs) < 0)
				oserror("Unable to block signals");

			if (!reap && !cull) {
				if (ppoll(pollfds, 1, NULL, &osigs) < 0 &&
				    errno != EINTR)
					oserror("Unable to suspend process");
			}

			if (sigprocmask(SIG_UNBLOCK, &sigs, NULL) < 0)
				oserror("Unable to unblock signals");

			read_cache_state();
		}

		/*
		 * If we need to check the consistency of the culling index,
		 * either at request from the kernel or from an earlier
		 * preliminary scan, fork off a process to handle this.
		 */
		if (state->need_fsck && !state->fsck_running) {
			/* cachefilesd_fork will set fsck_running = true. */
			rc = cachefilesd_fork(state);
			if (rc)
				error("Error creating scanning process.");
		}

		if (nocull) {
			cull = 0;
		} else {
			/* Is our queue empty? build a new one. */
			if (jumpstart_scan || (refresh & !cullq->ready)) {
				if (cullq->ready || cullq->youngest != -1 || cullq->oldest != 0) {
					debug(2, "Warning: jumpstart_scan ordered when table non-empty.");
					refresh = 1;
					continue;
				}

				jumpstart_scan = 0;
				refresh = 0;

				if (!stop) {
					debug(2, "Building Cull Queue.");
					timer_start(&tv);
					build_cull_queue(cullq, 1);
					usecs = timer_stop(&tv);
					debug(3, "Build time: %lu; oldest: %u, youngest: %u; ready: %d",
					      usecs, cullq->oldest, cullq->youngest, cullq->ready);

					/* Order a refresh of the queue in 30 secs. */
					signal(SIGALRM, sigalrm);
					alarm(refresh_rate);
				}
			}

			/* Is it time to refresh our queue? do so. */
			if (refresh) {
				refresh = 0;

				if (!cullq->ready) {
					debug(2, "Refresh requested, but queue not ready. ordering new build.");
					jumpstart_scan = 1;
					continue;
				}

				debug(3, "Refreshing queue");
				timer_start(&tv);
				queue_refresh(cullq);
				usecs = timer_stop(&tv);
				debug(3, "Refresh time: %lu; oldest: %u, youngest: %u; ready: %d",
				      usecs, cullq->oldest, cullq->youngest, cullq->ready);

				/* And make sure to order another refresh. */
				signal(SIGALRM, sigalrm);
				alarm(refresh_rate);
			}

			/* Do we need to cull something? */
			if (cull) {
				/* We've got stuff ready to cull: */
				if (cullq->ready) {
					debug(3, "Invoking cull_objects,");
					cull_objects(cullq);
					if (cullq->thrash > thrash_limit)
						error("Error: Can't find anything to cull! Giving up.");
					else if (cullq->thrash)
						debug(0, "Warning: thrashing... (%u)", cullq->thrash);
				}
				/* We need to find things to cull: */
				else {
					debug(3, "Cull requested, but table not ready.");
					jumpstart_scan = 1;
				}
			}
		}

		if (reap) {
			debug(3, "Cleaning the graveyard ...");
			reap_graveyard();
			debug(3, "...Done cleaning the graveyard.");
		}
	}

	cachefilesd_cleanup();
	notice("Daemon Terminated");
	exit(0);
}

/*****************************************************************************/
/*
 * check the graveyard directory for graves to delete
 */
static void reap_graveyard(void)
{
	/* set a one-shot notification to catch more graves appearing */
	reap = 0;
	signal(SIGIO, sigio);
	if (fcntl(graveyardfd, F_NOTIFY, DN_CREATE) < 0)
		oserror("unable to set notification on graveyard");

	reap_graveyard_aux(graveyardpath);
}

/*****************************************************************************/
/*
 * recursively remove dead stuff from the graveyard
 */
static void reap_graveyard_aux(const char *dirname)
{
	struct dirent dirent, *de;
	DIR *dir;
	int deleted, ret;

	if (chdir(dirname) < 0)
		oserror("chdir failed");

	dir = opendir(".");
	if (!dir)
		oserror("Unable to open grave dir %s", dirname);

	do {
		/* removing directory entries may cause us to skip when reading
		 * them */
		rewinddir(dir);
		deleted = 0;

		while (ret = readdir_r(dir, &dirent, &de),
		       ret == 0 && de != NULL) {
			/* ignore "." and ".." */
			if (dirent.d_name[0] == '.') {
				if (dirent.d_name[1] == '\0')
					continue;
				if (dirent.d_name[1] == '.' ||
				    dirent.d_name[1] == '\0')
					continue;
			}

			deleted = 1;

			/* attempt to unlink non-directory files */
			if (dirent.d_type != DT_DIR) {
				debug(1, "unlink %s", dirent.d_name);
				if (unlink(dirent.d_name) == 0)
					continue;
				if (errno != EISDIR)
					oserror("Unable to unlink file %s",
						dirent.d_name);
			}

			/* recurse into directories */
			memcpy(&dirent, de, sizeof(dirent));

			reap_graveyard_aux(dirent.d_name);

			/* which we then attempt to remove */
			debug(1, "rmdir %s", dirent.d_name);
			if (rmdir(dirent.d_name) < 0)
				oserror("Unable to remove dir %s",
					dirent.d_name);
		}

		if (ret < 0)
			oserror("Unable to read dir %s", dirname);
	} while (deleted);

	closedir(dir);

	if (chdir("..") < 0)
		oserror("Unable to chdir to ..");
}

/*****************************************************************************/
/*
 * read the cache state
 */
static void read_cache_state(void)
{
	char buffer[4096 + 1], *tok, *next, *arg;
	int n;

	debug(4,"read_cache_state();");
	n = read(cachefd, buffer, sizeof(buffer) - 1);
	if (n < 0)
		oserror("Unable to read cache state");
	buffer[n] = '\0';

	tok = buffer;
	do {
		next = strpbrk(tok, " \t");
		if (next)
			*next++ = '\0';

		arg = strchr(tok, '=');
		if (arg) {
			*arg++ = '\0';
		} else {
			debug(0, "Warning: malformed output from kernel, missing arg to [%s]", tok);
			continue;
		}

		if (strcmp(tok, "cull") == 0)
			cull = strtoul(arg, NULL, 0);
		if (strcmp(tok, "fsck") == 0)
			state->need_fsck |= strtoul(arg, NULL, 0);
		else if (strcmp(tok, "brun") == 0)
			brun = strtoull(arg, NULL, 16);
		else if (strcmp(tok, "bcull") == 0)
			bcull = strtoull(arg, NULL, 16);
		else if (strcmp(tok, "bstop") == 0)
			bstop = strtoull(arg, NULL, 16);
		else if (strcmp(tok, "frun") == 0)
			frun = strtoull(arg, NULL, 16);
		else if (strcmp(tok, "fcull") == 0)
			fcull = strtoull(arg, NULL, 16);
		else if (strcmp(tok, "fstop") == 0)
			fstop = strtoull(arg, NULL, 16);

	} while ((tok = next));
}


int destroy_file(int dirfd, struct dirent *de)
{
	char namebuf[40];
	static unsigned uniquifier;
	struct timeval tv;
	int rc;

	if (de->d_type != DT_DIR) {
		if (unlinkat(dirfd, de->d_name, 0) < 0 &&
		    errno != ENOENT) {
			rc = errno;
			debug(0, "Unable to unlink file: %s\n", de->d_name);
			return rc;
		}
	}
	else {
		gettimeofday(&tv, NULL);
		sprintf(namebuf, "x%lxx%xx", tv.tv_sec, uniquifier++);

		if (renameat(dirfd, de->d_name, graveyardfd, namebuf) < 0 &&
		    errno != ENOENT) {
			rc = errno;
			debug(0, "Unable to rename file: %s", de->d_name);
			return rc;
		}
	}

	return EXIT_SUCCESS;
}
