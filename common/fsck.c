/**
 * @file fsck.c
 */
#include "cachefilesd.h"
#include "debug.h"
#include "fsck.h"
#include "xattr.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <dirent.h>

#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/syscall.h>

#include <sys/mman.h>

/** fork_state is used to pass state to the SIGCHLD handler. */
cstate *fork_state = NULL;

/** Just to keep line lengths down. **/
typedef struct file_handle fht;

/******************************************************************************/

/* Macro-level fsck management. */
int cachefilesd_fsck_light (const char *cacheroot, cstate **state);
int cachefilesd_fsck_deep  (cstate *s, bool fork);
static int cachefilesd_fsck_impl  (cstate *s);
static int cachefilesd_fsck_end	  (cstate *s, int rc);

/* State Management */
static int  cachefilesd_fork	(cstate *s);
static void cachefilesd_sigchld (int sig);
static int  state_init		(cstate **state, const char *root);
void state_destroy	(cstate **state);
static int  init_fsck		(cstate *s);
static int  init_scan_state	(cstate *s);
static void destroy_scan_state	(cstate *s);
static int  open_indices	(cstate *s);
static int  close_indices	(cstate *s);

/* Scanning implementation */
static int read_state (cstate *s);
static int fsck_table (cstate *s);
static int fsck_page  (cstate *s);
static int fsck_slot  (cstate *s);
static int fsck_tree  (cstate *s, const char *relpath, int *num);
static int fsck_file  (cstate *s, int dirfd, const char *filename, struct stat64 *st);

/* Scanning Helpers */
static int  check_indices (cstate *s, bool *empty);
static int  check_fsizes  (cstate *s);
static int  load_page	  (cstate *s);
static int _load_page	  (cstate *s);
static int  save_page	  (cstate *s);
static int _save_page	  (cstate *s);
static bool is_empty	  (const struct index_record *r);
static bool is_real_empty (const struct scan_state *t);
static bool is_dotdir	  (const char *d_name);
static bool is_goodtype	  (int d_type);
static bool is_expected	  (const char *d_name, mode_t mode);
static int  get_fd_at	  (const char *root, const struct index_record *rec);
static fht *ntha	  (int dirfd, const char *name);
static int  get_handle	  (int dirfd, const char *name, bool isdir,
			   fht **return_fh);
static int read_slot	  (const char *filename, slot_t *slot);
static int read_slot_fd	  (int fd, slot_t *slot);
static int record_seek	  (cstate *s, slot_t slot_no);
static int page_seek	  (cstate *s, size_t pageno);

/* Remedial Actions */
static int delete_slot (cstate *s, slot_t slot_no);
static int repair_slot (cstate *s, slot_t slot_no, struct cache_xattr_usr *x);
static int delete_file (cstate *s, int dirfd, const char *name);
static int delete_dir  (cstate *s, const char *dirname);

/* Remedial Implementations */
static int _delete_slot_offline (cstate *s, slot_t slot_no);
static int _delete_slot_online	(slot_t slot_no);
static int _repair_slot_offline (slot_t slot_no, struct cache_xattr_usr *x);
static int _repair_slot_online	(slot_t slot_no);
static int _delete_file_offline (int dirfd, const char *name, const struct stat64 *st);
static int _delete_file_online	(const char *filename);
static int _delete_dir_offline	(cstate *s, const char *dirname);

/* Utility. */
static char  *str2cat	       (const char *a, const char *b);
static size_t mult_ceil	       (size_t n, size_t multiple);
static size_t mult_floor       (size_t n, size_t multiple);
static int    check_state      (const cstate *s);
static int    check_scan_state (const cstate *s);
static void   print_record     (const struct scan_state *t);
static void   print_handle     (const struct file_handle *fh);
static void   print_xattr      (const struct cache_xattr_usr *x);
static bool   empty_dir	       (const char *dirname);
static bool   empty_dir_dh     (DIR *dh);
static struct index_record *get_record (const cstate *s);
static struct index_record *record     (const struct scan_state *t);
static struct scan_state   *scanstate  (const cstate *s);

/******************************************************************************/
/* Macro-level scan management.						      */
/******************************************************************************/

/**
 * cachefilesd_fsck_light initializes the cachefilesd_state structure,
 * performs a preliminary scan, and makes a note if a closer inspection
 * is warranted. How (and if) the deep scan is launched is up to the
 * caller, by investigating (*state)->need_fsck.
 * @param cacheroot The root directory of the cache to scan.
 * @param[out] state A handle to some fsck-info in case the user decides
 * to issue a deep fsck.
 * @return An errno-compatible int.
 * @sideeffect fsck state is accumulated into *state.
 * @see cachefilesd_state_destroy
 */
int cachefilesd_fsck_light(const char *cacheroot, cstate **state)
{
	int rc;

	if (state == NULL)
		return EINVAL;

	if (*state == NULL) {
		rc = state_init(state, cacheroot);
		ret_usrerr((rc != 0), "Failed to initialize cachefilesd state structure.");
	}

	/* clang workaround. */
	if (!(*state && (*state)->indexfile && (*state)->atimefile)) {
		error("Internal error; failed to allocate buffers for index names.");
	}

	info("Scan started.\n"
	     "Cacheroot: [%s]\n"
	     "Index: [%s]\n"
	     "Atimes: [%s]\n",
	     cacheroot,
	     (*state)->indexfile,
	     (*state)->atimefile);

	/* Retrieve filesizes and cache-wide xattrs. */
	rc = read_state(*state);
	ret_usrerr((rc != 0), "Error: Failed to retrieve cachefilesd state.\n"
		   "\tCache index may be corrupt. Unable to verify.");

	/* It's up to the user to request a deep scan based on the result from
	 * (*state)->need_fsck, because we don't necessarily know if they
	 * want to run it in the background, exit, or whatever else. */
	if (!(*state)->need_fsck)
		info("Cache appears clean.");

	return 0;
}


/**
 * cachefilesd_fsck_deep is a function that encapsulates both
 * fsck_table (fixing the index) and fsck_tree (fixing the files.)
 * Optionally, it may be launched as a separate process in the background.
 * @param s The cachefilesd state structure to work on
 * @param fork Should we launch this as a separate process?
 * @return An errno-compatible integer.
 * @note If fork is set to true, the return code will only be
 * indicative of a successful launch, not of scan's completion.
 * @see cachefilesd_fork
 */
int cachefilesd_fsck_deep  (struct cachefilesd_state *s, bool fork)
{
	return fork ? cachefilesd_fork(s) : cachefilesd_fsck_impl(s);
}


/**
 * cachefilesd_fsck_impl is a function that implements fsck_deep,
 * which encapsulates both fsck_table (which fixes the index) and
 * fsck_tree (which fixes the files.)
 * @param s The cachefilesd state structure.
 * @return An errno-compatible integer.
 */
static int cachefilesd_fsck_impl(cstate *s)
{
	int rc, num;

	if ((rc = check_state(s)))
		return rc;

	/* Read in the state, create the scan state structure,
	 * and open the index files for reading/writing. */
	rc = init_fsck(s);
	jmp_usrerr((rc), closeout, "Failed to initialize deep scan.");

	/* If there was no error but the state is still not 'read',
	 * this means the indices are empty, which is OK. */
	if (s->read) {
		info("[1/3] Checking consistency of culling index.");
		rc = fsck_table(s);
		jmp_usrerr((rc), closeout, "Failed to scan the culling index.");

		sync();
	} else {
		info("[1/3] Skipping consistency check of culling index because I couldn't find it.");
	}

	/* Jump to the top of the cache */
	rc = chdir(s->rootdir);
	jmp_syserr((rc), closeout,
		   "Failed to change directory to [%s]", s->rootdir);

	/* Check for orphaned and junk files in the cache. If the directory
	 * does not exist, that's OK. It might not exist yet. */
	rc = access("cache", F_OK);
	jmp_syserr((rc != 0) && (errno != ENOENT), closeout,
		   "Could not access the cache directory");
	if (rc == 0) {
		info("[2/3] Checking consistency of files in cache directory.");
		rc = fsck_tree(s, "cache", &num);
		jmp_usrerr((rc), closeout, "Spidering through the cachedir failed.");
	} else {
		info("[2/3] Skipping cache files consistency check because the cache dir does not exist.");
	}

	/* Duplicate detection run and cleanup of the index after file deletions */
	if (s->read) {
		info("[3/3] Checking consistency of culling index.");
		s->scan->duplicate_pass = 1;
		rc = fsck_table(s);
		jmp_usrerr((rc), closeout, "Failed to scan the culling index.");

		sync();
	} else {
		info("[3/3] Skipping consistency check of culling index because I couldn't find it.");
	}

	rc = 0;
closeout:

	/* close out the scan: either delete the .lockfile or
	 * send a message to the kernel about how we did. */
	rc = cachefilesd_fsck_end(s, rc);
	ret_usrerr((rc != 0), "Error in post-scan cleanup.");

	return 0;
}


/**
 * cachefilesd_fsck_end will clean up after a scan:
 * depending on if we are bound to the kernel or not,
 * we will either delete the lockfile or report our
 * status to the kernel as appropriate.
 * @param s The cachefilesd_state.
 * @param res The resulting error code from the scan.
 * @return One of two things:
 * (A) A new errno-compatible return code, encountered here, or
 * (B) res is returned back to the caller.
 */
static int cachefilesd_fsck_end(struct cachefilesd_state *s, int res)
{
	int rc;

	if ((rc = check_state(s)))
		return rc;

	info("Scan finished, return = %d.", res);
	if (s->scan) {
		debug(2, "Number of Fixes: %lu; Page loads: %lu.",
		      s->scan->fixes, s->scan->loads);
	}

	/* If we didn't engage the kernel and found no errors,
	 * delete the .lock file. Otherwise, we assume the kernel
	 * is active and expects the lock file to be present. */
	if (!res && !s->bound) {
		rc = chdir(s->rootdir);
		ret_syserr((rc != 0),
			   "Failed to change directories to the root dir (%s)",
			   s->rootdir);

		rc = unlink(".lock");
		ret_syserr((rc && errno != ENOENT),
			   "Failed to unlink .lock file");
	}

	/* If we are running 'online,' bound to the kernel module,
	 * Tell the kernel the results of the scan. */
	if (s->bound) {
		unsigned char len, code = res;
		char sbuff[10];

		len = snprintf(sbuff, 10, "fsck %d", code);
		debug(3, "sending cmd: [%s]", sbuff);
		rc = write(cachefd, sbuff, len);
		ret_syserr((rc < len),
			   "Error reporting status (cmd=[%s]) to the kernel",
			   sbuff);
	}

	/* Clean up any scanning remnants. */
	destroy_scan_state(s);

	return res;
}

/******************************************************************************/
/* State management.							      */
/******************************************************************************/

/**
 * cachefilesd_fork will fork off a process to perform a deep scan
 * of the culling index, and manage the needed state to do so.
 * @param s The cachefiles cache to fork off with and scan.
 * @return An errno compatible return code.
 * @note that a successful return code only indicates that we
 * succeeded in forking off the scan, not that it went OK.
 * @sideeffect A bookmark to the passed cachefilesd_state, s,
 * is saved in a global variable named fork_state.
 */
static int cachefilesd_fork(cstate *s)
{
	pid_t pid;
	int rc;

	if ((rc = check_state(s)))
		return rc;

	/* bookmark this state struct globally for the sighandler */
	fork_state = s;

	/* Register the sighandler */
	if (signal(SIGCHLD, cachefilesd_sigchld) == SIG_ERR) {
		rc = errno;
		dperror("Failed to register signal handler for fsck process");
		return rc;
	}

	if (s->fsck_running) {
		debug(0, "Can't start a scan while one is already running.");
		return EINVAL;
	}

	s->fsck_running = 1;
	debug(2, "in fork() -- parent pid = %u", getpid());

	switch ((pid = fork())) {
	case -1:
		rc = errno;
		dperror("Failed to fork the fsck process.");
		s->fsck_running = 0;
		return rc;
	case 0:
		/* "Don't let the kid outta the box" -- ~bill */
		debug(2, "in fork() -- child pid = %u", getpid());
		if (setpriority(PRIO_PROCESS, syscall(SYS_gettid), 19)) {
			rc = errno;
			dperror("Failed to adjust scanning process niceness");
			exit(rc);
		}

		rc = cachefilesd_fsck_impl(s);
		if (rc)
			debug(0, "Failed to complete deep cachefilesd fsck.");

		cachefilesd_cleanup();
		exit(rc);
	default:
		debug(2, "Launched scanning process. pid=%u", pid);
		return 0;
	}
}


/**
 * signal handler for SIGCHLD.
 * Will adjust state accordingly to indicate that the scan is now finished.
 * @param sig The incoming signal. Expected to be SIGCHLD.
 * @note Refers to the bookmarked cachefilesd_state fork_state.
 * @sideeffect If an error is encountered, the global stop will be set to 1.
 */
static void cachefilesd_sigchld(int sig)
{
	pid_t pid;
	int rc = -1;

	if (sig != SIGCHLD) {
		debug(0, "SIGCHLD signal handler called with inappropriate signal [%d]",
		      sig);
		goto error;
	}

	pid = wait(&rc);
	if (pid == -1) {
		dperror("Failed to wait() on child in sighandler");
		goto error;
	}
	debug(1, "fsck process (%u) exited. got rc = %d", pid, rc);

	if (!fork_state) {
		debug(0, "Error: a child process has exited, but I can't map its PID to a cache.");
		goto error;
	}

	if (!rc) {
		debug(1, "fsck completed successfully.");
		goto leave;
	} else {
		debug(0, "Error: fsck encountered problems. Halting daemon.");
		goto error;
	}

error:
	stop = 1;
leave:
	if (fork_state) {
		/* Mark as zero regardless so we don't spawn an extra scan
		 * if we are exiting due to error. */
		fork_state->need_fsck = 0;
		fork_state->fsck_running = 0;
	}
}


/**
 * state_init initializes the cachefilesd_state structure
 * by performing the menial tasks of allocating strings
 * and so forth that point to paths of interest.
 * @param[out] state A handle to the state pointer, so we can allocate the structure.
 * @param root A const string indicating the fscache root directory.
 * @see state_destroy for cleanup.
 * @return An errno-compatible integer.
 */
static int state_init(struct cachefilesd_state **state, const char *root)
{
	int rc;
	struct cachefilesd_state *s;

	if (!state) {
		debug(0, "state_init requires a valid (state **) pointer.");
		rc = EINVAL;
		goto leave;
	}
	if (*state) {
		debug(0, "state_init expects a pointer to a NULL value.");
		debug(0, "We might leak memory otherwise!");
		rc = EINVAL;
		goto leave;
	}

	s = malloc(sizeof(struct cachefilesd_state));
	jmp_syserr((s == NULL), leave,
		   "Could not allocate space for cachefilesd_state structure");

	s->rootdir = root;

	s->indexfile = str2cat(root, "/cull_index");
	jmp_syserr((s->indexfile == NULL), error,
		   "Failed to generate indexfile string");

	s->atimefile = str2cat(root, "/cull_atimes");
	jmp_syserr((s->atimefile == NULL), error2,
		   "Failted to generate atimefile string");

	s->scan = NULL;
	s->pagesize = getpagesize();
	s->num_indices = 0;
	s->num_perpage = 0;
	s->ent_size = 0;

	/* booleans */
	s->read = 0;
	s->bound = 0;
	s->need_fsck = 0;
	s->fsck_running = 0;

	s->init = 1;
	*state = s;
	return 0;

error2:
	free(s->indexfile);
error:
	free(s);
leave:
	return rc;
}


/**
 * state_destroy frees any memory and closes any open handles
 * that were opened by state_init, or the subsequent scan(s).
 * @param[in,out] state A pointer to the handle of the cachefilesd_state structure to destroy.
 * @return void. Any errors will terminate the program.
 */
void state_destroy(struct cachefilesd_state **state)
{
	struct cachefilesd_state *s;

	if (!state) return;
	if ((s = *state) == NULL) return;

	if (s->scan) {
		destroy_scan_state(s);
		s->scan = NULL; /* dead horse */
	}

	free(s->indexfile);
	s->indexfile = NULL;

	free(s->atimefile);
	s->atimefile = NULL;

	free(s);
	*state = NULL;
}


/**
 * init_fsck initializes on-demand a few things in one shot.
 * (1) We read the state of the cache (If we haven't yet),
 * (2) We create the scanning_state structure and buffers, and
 * (3) We open the culling indices for reading.
 * @param s An initialized cachefilesd_state.
 * @return An errno-compatible integer.
 */
static int init_fsck(cstate *s)
{
	int rc = check_state(s);
	if (rc) return rc;

	/* If the state hasn't been read in yet, do so. */
	if (!s->read) {
		rc = read_state(s);
		ret_usrerr((rc != 0),
			   "Failed to read state on-demand during deep scan.");
	}

	/* Initialize the scan_state structure */
	rc = init_scan_state(s);
	ret_usrerr((rc != 0),
		   "Failed to initialize scan_state structure.");

	/* Open up the files for reading/writing ... */
	if (s->read) {
		rc = open_indices(s);
		jmp_usrerr((rc != 0), error,
			   "Failed to open culling indices.");
	} else {
		info("Not opening indices; state has not been read.");
	}

	return 0;
error:
	destroy_scan_state(s);
	return rc;
}


/**
 * init_scan_state will initialise or re-initialise a scanning state structure.
 * @param s The cachefilesd state to attach the new scanning state to
 * @return An errno-compatible integer.
 */
static int init_scan_state(struct cachefilesd_state *s)
{
	struct scan_state *t;
	int rc = 0;
	bool new = 0;

	if ((rc = check_state(s)))
		return rc;

	/* Create space for the scan ... */
	if ((t = s->scan) == NULL) {
		new = 1;
		t = calloc(sizeof(struct scan_state), 1);
		ret_syserr((t == NULL),
			   "Failed to allocated scan state structure");
	}

#ifdef BUFFER
	/* Create a buffer for the atimes ... */
	if (!t->abuffer) {
		t->abuffer = calloc(sizeof(atime_t), s->num_perpage);
		jmp_syserr((t->abuffer == NULL), free_once,
			   "Failed to allocate space for atimes array");
	}

	/* Create a buffer for the entities ... */
	if (!t->buffer) {
		t->buffer = calloc(s->ent_size, s->num_perpage);
		jmp_syserr((t->buffer == NULL), free_both,
			   "Failed to allocate space for entity page");
	}
#endif

	if (new) {
		t->duplicate_pass = 0;
		t->indices_open = 0;
		t->page_loaded = 0;
		t->dirty = 0;
		t->fixes = 0;
		t->loads = 0;
		t->r = NULL;
#ifndef BUFFER
		/* mappings, file handles */
		t->buffer = NULL;
		t->abuffer = NULL;
		t->map_delta = 0;
		t->atime_fd = -1;
		t->index_fd = -1;
#else
		t->atimefh = NULL;
		t->indexfh = NULL;
#endif
		/* connect the structures */
		s->scan = t;
	}

	return 0;
#ifdef BUFFER
free_both:
	free(t->abuffer);
	t->abuffer = NULL;
free_once:
	free(t);
	s->scan = NULL;
	return rc;
#endif
}


/**
 * destroy_scan_state will clean up the scanning-related portions of the state.
 * @param s The cache to destroy the scanning state of.
 * @sideeffect The program will exit if there are filesystem errors.
 */
static void destroy_scan_state(struct cachefilesd_state *s)
{
	struct scan_state *t;

	if (!s) return;
	if ((t = s->scan) == NULL) return;

	/* Just in case, close out our scanning buffers. */
	save_page(s);

#ifdef BUFFER
	free(t->buffer);
	t->buffer = NULL;

	free(t->abuffer);
	t->abuffer = NULL;
#endif
	if (close_indices(s))
		error("Failed to close indices on destruction of scan_state.");

	free(t);
	s->scan = NULL;
}


/**
 * open_indices will open up the culling indices for reading.
 * If either are already open, no actions will be performed.
 * @param s The cache whose indices are to be opened
 * @return An errno-compatible integer.
 */
static int open_indices(struct cachefilesd_state *s)
{
	int rc;
	struct scan_state *t = scanstate(s);

	if ((rc = check_scan_state(s)))
		return rc;

#ifndef BUFFER
#define _open(FD,NAME) if ((FD) < 0) {					\
		FD = open((NAME), O_RDWR);				\
		jmp_syserr((FD < 0), error,				\
			   "Failed to open an index file: [%s]", (NAME)); \
	}
	_open(t->index_fd, s->indexfile);
	_open(t->atime_fd, s->atimefile);
#else
#define _open(FH,NAME) if (!FH) {					\
		(FH) = fopen((NAME), "r+");				\
		jmp_syserr((FH == NULL), error,				\
			   "Failed to open an index file: [%s]", (NAME)); \
	}
	_open(t->indexfh, s->indexfile);
	_open(t->atimefh, s->atimefile);
#endif

	t->indices_open = 1;
	return 0;
error:
	if (close_indices(s))
		error("Error closing files in error handler.");
	return rc;
}


/**
 * close_indices will close any open file handles to the culling indices.
 * @param s The cache whose files we should close.
 * @return An errno-compatible integer.
 */
static int close_indices(struct cachefilesd_state *s)
{
	int rc;
	struct scan_state *t = scanstate(s);

	if ((rc = check_scan_state(s)))
		return rc;

#ifdef BUFFER
	if (t->atimefh) {
		rc = fclose(t->atimefh);
		ret_syserr((rc != 0), "Failed to close atimes file [%s]", s->atimefile);
		t->atimefh = NULL;
	}

	if (t->indexfh) {
		rc = fclose(t->indexfh);
		ret_syserr((rc != 0), "Failed to close index file [%s]", s->indexfile);
		t->indexfh = NULL;
	}
#else
	if (t->atime_fd >= 0) {
		rc = close(t->atime_fd);
		ret_syserr((rc != 0), "Failed to close atime file [%s], fd [%d]",
			   s->atimefile, t->atime_fd);
		t->atime_fd = -1;
	}

	if (t->index_fd >= 0) {
		rc = close(t->index_fd);
		ret_syserr((rc != 0), "Failed to close index file [%s], fd [%d]",
			   s->indexfile, t->index_fd);
		t->index_fd = -1;
	}
#endif

	t->indices_open = 0;
	return 0;
}

/******************************************************************************/
/* Scanning implementation.						      */
/******************************************************************************/

/**
 * Retrieve basic cache statistics,
 * including xattrs, filesize, and so on.
 * Perform basic sanity checks on xattrs,
 * filesizes, and so on.
 *
 * @param s The cache to operate on.
 * s->indexfile and s->atimefile must be set.
 * @return An errno-compatible integer.
 */
static int read_state(struct cachefilesd_state *s)
{
	int rc = 0;
	bool empty;

	if ((rc = check_state(s)))
		return rc;

	/* The state, as of now, is only semi-correct and not properly read. */
	s->read = 0;

	/* Check for presence of .lock file */
	chdir(s->rootdir);
	if ((rc = access(".lock", F_OK)) == 0 && !s->bound) {
		/* If we haven't bound to the cache yet,
		 * but there is a .lockfile, it is stale. */
		warning("Stale .lock file detected.");
		s->need_fsck = 1;
	}
	ret_syserr((rc && errno != ENOENT), "Error checking for .lock file");

	/* Check on presence & accessibility cull_index and cull_atimes */
	rc = check_indices(s, &empty);
	ret_usrerr((rc != 0), "Error looking up cache index files");
	/* If both of the indices are not present, that's OK. */
	if (empty) return 0;


	/* Retrieve the entity size */
	rc = gx(s->indexfile, index_xattr, "%02x", &s->ent_size);
	ret_usrerr((rc != 0), "gx(%s) failed.", s->indexfile);
	if (s->ent_size == 0) {
		debug(0, "The stored xattr size in the culling index CANNOT be zero.");
		return EINVAL;
	}

	/* Retrieve the atime_base. Make sure to use the right format and
	 * vartype, or gremlins will eat your stack. */
	if ((rc = gx(s->atimefile, atime_xattr,
		     "%016lx", &s->atime_base))) {
		/* Soft error: We don't need this, and the kernel will fix it. */
		warning("error retrieving atime_base from s->atimefile.");
		s->atime_base = 0;
		s->need_fsck = 1;
	}

	/* Check for agreement in number of entries, index lengths, etc. */
	rc = check_fsizes(s);
	ret_usrerr((rc != 0), "Error determining index lengths.");

	debug(2, "entsize: %d; atime_base: %lu\n"
	      "index size: %lu; atimes size: %lu\n"
	      "perpage: %d; num: %d; anum: %d",
	      s->ent_size, s->atime_base, s->index_size, s->atime_size,
	      s->num_perpage, s->num_indices, s->num_atimes);

	/* the state has now been properly read. */
	s->pagesize = getpagesize();
	s->read = 1;
	return 0;
}


/**
 * fsck_table verifies all of the indices in the culling index.
 * @param s The state structure describing the index to scan.
 * @return An errno-compatible integer.
 */
static int fsck_table(cstate *s)
{
	int rc;
	size_t i, npages = (s->index_size / s->pagesize);

	if ((rc = check_scan_state(s)))
		return rc;

	/* Read both indices, page-by-page. */
	for (i = 0; i < npages; ++i) {
		/* On-demand load the page we want. */
		rc = page_seek(s, i);
		jmp_usrerr((rc != 0), close, "Failed to load page #%lu.", i);

		/* Do our error-checking on this chunk. */
		rc = fsck_page(s);
		jmp_usrerr((rc != 0), close, "Error analyzing/repairing page #%lu.", i);
	}

	return 0;
close:
	return rc;
}


/**
 * fsck_page confirms the validity of each index entry,
 * one page's worth of index entries at a time.
 * @param s The cachefilesd state structure.
 * @return An errno-compatible integer.
 */
static int fsck_page(cstate *s)
{
#define record_at(D,J) (D[(J)*(s->ent_size)])

	unsigned j;
	int rc;
	struct scan_state *t = scanstate(s);

	if ((rc = check_scan_state(s)))
		return rc;

	/* Notate our starting index number ... */
	t->index = t->pageno * s->num_perpage;

	/* loop through the records in the page. */
	for (j = 0; j < s->num_perpage; ++j, ++t->index) {
		t->local_index = j;
		t->r = (struct index_record *) &(record_at(t->buffer,j));

		/* Nothing to do if it is empty. */
		if (!is_real_empty(t)) {
			rc = fsck_slot(s);
			jmp_usrerr((rc != 0), error, "Error fixing slot #%u", t->index);
		}
	}

	/* Note: no need to save the page manually here: if another page is
	 * loaded, we'll save this one automatically. OR, we'll save it on exit. */
	t->r = NULL;
	return 0;
error:
	debug(0, "Error encountered scanning page #%lu", t->pageno);
	t->r = NULL;
	return rc;
}


/**
 * fsck_slot will, given an index, verify that:
 * (1) the indexed entry points to a valid file
 * (2) That file has an xattr,
 * (3) That xattr contains the correct index.
 *
 * In the negative case of (1) and (2), we blank the index entry.
 * In the case of (3), we repair the xattr.
 *
 * @param s Contains cache information, buffers and statistics.
 * @return An errno-compatible integer.
 */
static int fsck_slot(cstate *s)
{
	int fd, rc;
	struct scan_state *t = scanstate(s);
	struct index_record *r = record(t);
	struct cache_xattr_usr *x = NULL;

	/* Sanity check arguments, as always... */
	if ((rc = check_scan_state(s))) return rc;
	if (r == NULL) return EINVAL;

	if (!t->duplicate_pass) {
		print_record(t);
	}

	/* Check for emptiness consistency. */
	if (is_empty(t->r) && t->abuffer[t->local_index]) {
		debug(1, "Index inconsistency: slot (%u) is unused but atime is non-zero. (%u)",
		      t->index, t->abuffer[t->local_index]);
		rc = delete_slot(s, t->index);
		jmp_usrerr((rc != 0), leave,
			   "Slot #%u is in an inconsistent state (atime is non-zero),\n"
			   "But I was unable to re-zero out the entry.", t->index);
		goto leave;
	}

	/* convert the handle to a file descriptor */
	fd = get_fd_at(s->rootdir, r);
	jmp_syserr((fd < 0 && errno != ESTALE), leave,
		   "Failed to open file via its handle.");
	if (errno == ESTALE) {
		debug(1, "Stale file handle in index: Deleting slot #%u.", t->index);
		rc = delete_slot(s, t->index);
		jmp_usrerr((rc != 0), leave, "Error deleting slot #%u.", t->index);
		goto leave;
	}

	/* Obtain the xattrs of the open file */
	rc = _bx(fd, slot_xattr, (struct generic_xattr **)&x);
	jmp_usrerr((rc && errno != ENOATTR), close1,
		   "Error obtaining xattrs for slot object #%u", t->index);
	if (rc == ENOATTR) {
		debug(1, "Suspected stale filehandle: slot #%u points to a "
		      "file with missing xattr property. Deleting slot.", t->index);
		rc = delete_slot(s, t->index);
		goto close1;
	}

	/* Debug: Print the entire xattr for viewing. */
	print_xattr(x);

	/* If the index matches the xattr, we're all set. */
	if (t->index == x->cachedata.cullslot) {
		rc = 0;
		goto free1;
	}

	/* Clear the file out of the index if it is PINNED. */
	if (x->cachedata.cullslot == CACHEFILES_PINNED) {
		debug(1, "Slot #%u points to a PINNED file. Removing this slot.",
		      t->index);
		rc = delete_slot(s, t->index);
		jmp_usrerr((rc != 0), free1, "Error clearing pinned file from index.");
		goto free1;
	}

	/* If this file points to the wrong slot, but we're on our second pass,
	 * this means that this entry is a duplicate and can be removed. */
	if (t->duplicate_pass) {
		debug(1, "Slot #%u points to the wrong slot (#%u), this slot is likely a duplicate.",
		      t->index, x->cachedata.cullslot);
		rc = delete_slot(s, t->index);
		jmp_usrerr((rc != 0), free1, "Error clearing duplicate slot from index.");
	} else {
		/* Otherwise, try to repair it. */
		x->xattr.fd = fd;
		debug(1, "Slot #%u points to a file which "
		      "points back to slot #%u. Correcting xattrs.\n",
		      t->index, x->cachedata.cullslot);

		rc = repair_slot(s, t->index, x);
		jmp_usrerr((rc != 0), free1, "repair_slot did not succeed.");
	}

free1:
	free(x);
close1:
	close(fd);
leave:
	return rc;
}


/**
 * Given a directory name, iterate through the objects in that directory,
 * validating files and recursing on further directories found.
 * @param s cachefilesd_state object describing the folders to spider through
 * @param relpath A directory relative to the CWD to enter and scan.
 * @param[out] num Returns the number of files remaining in this directory.
 * @return An errno-compatible integer.
 */
int fsck_tree(cstate *s, const char *relpath, int *num)
{
	DIR *dh;
	struct dirent dirent, *de;
	struct stat64 st;
	int rc;

	/* Count the number of files we find, to delete empty dirs later. */
	*num = 0;

	/* Open up this directory. */
	dh = opendir(relpath);
	ret_syserr((dh == NULL && errno != ENOENT), "Couldn't open directory");
	if (dh == NULL && errno == ENOENT) {
		debug(1, "Warning: [%s] went missing", relpath);
		return 0;
	}

	/* Change CWD to this directory. */
	rc = fchdir(dirfd(dh));
	jmp_syserr((rc == -1), err_close, "Failed to change CWD to [%s]", relpath);

	/* In general, keep reading files until we can't. */
	while (1) {

		/* Iterate through this directory. */
		rc = readdir_r(dh, &dirent, &de);
		jmp_syserr((rc < 0 && errno != ENOENT), err_close,
			   "Error, Problem reading directory");
		if (rc < 0 && errno == ENOENT) {
			debug(1, "Warning: File was already deleted.");
			break;
		}

		/* No more files. */
		if (de == NULL) break;

		/* Skip . and .. */
		if (is_dotdir(dirent.d_name)) continue;

		/* At this point, we know this dir isn't empty. */
		(*num)++;

		/* We accept 'unknown', 'dir' and 'reg' files */
		if (!is_goodtype(dirent.d_type)) {
			debug(1, "Warning: Unknown d_type: %d",
			      dirent.d_type);
			continue;
		}

		/* Fetch stats */
		rc = fstatat64(dirfd(dh), dirent.d_name, &st, 0);
		jmp_syserr((rc && errno != ENOENT), err_close,
			   "Error, Failed to stat directory [%s]",
			   dirent.d_name);
		if (rc && errno == ENOENT) {
			debug(1, "Notice: File [%s] disappeared prior to stat call.",
			      dirent.d_name);
			(*num)--;
			continue;
		}

		/* Make sure filename looks correct,
		 * and ensure the prefix letter matches the actual filetype. */
		if (!is_expected(dirent.d_name, st.st_mode)) {
			debug(1, "[%s] has a bad name, or bad name/type combo. Deleting.",
			      de->d_name);
			rc = delete_file(s, dirfd(dh), de->d_name);
			if (rc) {
				debug(1, "Warning: Could not remove clutter file [%s]",
				      de->d_name);
			} else {
				(*num)--;
			}

			continue;
		}

		/* Descend first, if applicable. */
		if (S_ISDIR(st.st_mode)) {
			int child_num;
			rc = fsck_tree(s, de->d_name, &child_num);
			if (rc)
				goto err_close;

			/* Assume that if fsck_tree() reports 0 entries,
			 * that the child routine deleted this directory,
			 * and continue to the next dir entry. */
			if (child_num == 0) {
				(*num)--;
				continue;
			}
		}

		/* If we've made it this far, we've got a candidate file to
		 * scan for validity. Pass our cache-wide struct s, the DIR
		 * handle, and the dirent, and the states over. */
		rc = fsck_file(s, dirfd(dh), de->d_name, &st);
		if (rc == EEXIST) {
			/* EEXIST: i.e, "This file exists, but shouldn't. */
			rc = delete_file(s, dirfd(dh), de->d_name);
			jmp_usrerr((rc != 0), err_close,
				   "Failed to delete %s.", de->d_name);
			(*num)--;
		} else if (rc) {
			debug(0, "fsck_file failed on %s", de->d_name);
			goto err_close;
		}
	}

	/* At this point, we're done iterating through this folder.
	 * Move back up one directory, so that our parent is
	 * where it left off. */
	closedir(dh);
	rc = chdir("..");
	ret_syserr((rc == -1), "Couldn't move back up the directory tree");

	/* If we found the directory was empty, kill it.
	 * delete_dir provides some safeguards against deleting
	 * non-empty directories, but why tempt fate? */

	if (!*num) {
		debug(1, "Removing empty directory (%s)", relpath);
		rc = delete_dir(s, relpath);
		if (rc) {
			debug(0, "Unable to remove directory presumed to be empty. (%s)", relpath);
			return rc;
		}
	}

	return 0;
err_close:
	closedir(dh);
	return rc;
}


/**
 * fsck_file verifies, from the point of view of the file,
 * that everything appears to be OK.
 * We will verify that we have sane xattrs and that our
 * xattr points back to the correct slot in the index,
 * which means that our filehandle matches the one
 * stored in the index.
 *
 * Anything that does not align properly is deleted,
 * under the assumption that anything incorrect is an
 * orphaned file, because any simple inconsistencies
 * were previously fixed by a call to fsck_table.
 *
 * @param s The current scan status/state. Needed for reading the index.
 * @param dirfd An open handle to the current scanning diretcory.
 * @param filename The file to validate.
 * @param st a stat64 structure generated on the file previously.
 *
 * @return An errno-compatible integer,
 * Potentially EEXIST if the file should be deleted.
 */
int fsck_file(cstate *s, int dirfd, const char *filename,
	      struct stat64 *st)
{
	int rc;
	slot_t slot = CACHEFILES_NO_CULL_SLOT;
	struct index_record *r;
	struct file_handle *fh = NULL;

	rc = check_scan_state(s);
	if (rc) return rc;

	/* Intermediate nodes have no data, that's OK. See you later! */
	if ((filename[0] == '@') || (filename[0] == '+')) {
		debug(3, "Skipping [%s]: Intermediate index.", filename);
		return 0;
	}

	/* Grab the xattr slot from the file */
	rc = read_slot(filename, &slot);
	if (rc == ENOATTR) {
		debug(1, "[%s] doesn't have the correct xattrs.", filename);
		return EEXIST; /* Signal to delete this file. */
	}
	ret_usrerr((rc != 0), "Error retrieving xattrs from file.");
	debug(2, "slot: %03u; file: [%s]; ", slot, filename);

	/* PINNED is okay. */
	if (slot == CACHEFILES_PINNED) {
		debug(3, "[%s] is pinned\n", filename);
		return 0;
	}

	/* Out of bounds cullslot, including NO_CULL_SLOT. */
	if (slot > s->num_indices) {
		debug(1, "File has an out-of-bounds cullslot (%u > %u)",
		      slot, s->num_indices);
		return EEXIST;
	}

	/* Retrieve an encoded file_handle for this object */
	rc = get_handle(dirfd, filename, S_ISDIR(st->st_mode), &fh);
	ret_usrerr((rc != 0 || fh == NULL),
		   "Failed to compile file_handle for [%s]", filename);
	print_handle(fh);

	/* Note, we overbuffer on-demand, hoping it is faster than seeking individual records. */
	rc = record_seek(s, slot);
	jmp_usrerr((rc != 0), free_fh, "Error retrieving index record in fsck_file.");
	r = s->scan->r;

	/* Check for agreement between our file handle and our slot's stored file handle. */
	if (memcmp(r->fh, fh->f_handle, fh->handle_bytes) != 0) {
		debug(1, "Error: file_handles differ. Removing object.\n");
		rc = EEXIST;
		goto free_fh;
	}

	rc = 0;
free_fh:
	free(fh);
	return rc;
}

/******************************************************************************/
/* Scanning Helpers.							      */
/******************************************************************************/

/**
 * check_indices will ensure that the index files are present, if possible.
 * If only one of both files are missing, it will attempt to fix the situation
 * by either deleting the atimes file, or creating a new atimes file.
 * If corrective measures fail, an error code is returned.
 * If the files are present, 'empty' will be set to false.
 * @param s The cache to check the indices for
 * @param[out] empty A return value pointer that indicates if the files were present.
 *
 * @return An errno-compatible integer.
 */
static int check_indices(cstate *s, bool *empty)
{
	int a, b, rc = 0;

	if (empty == NULL) return EINVAL;
	*empty = false;

	/* Check for presence of index and atimes files.
	 * If they aren't here, that's OK! */
	if ((a = access(s->indexfile, F_OK)) == -1) {
		ret_syserr((errno != ENOENT), "Failed to access [%s]", s->indexfile);
	}
	if ((b = access(s->atimefile, F_OK)) == -1) {
		ret_syserr((errno != ENOENT), "Failed to access [%s]", s->atimefile);
	}

	/* If both files don't exist, that's OK. */
	if ((a == -1) && (b == -1)) {
		*empty = 1;
		return 0;
	} else if (a == -1) {
		info("Index is missing, removing vestigial cull_atimes file.");
		rc = unlink(s->atimefile);
		ret_syserr((rc != 0), "Failed to remove vestigial cull_atimes file");
		*empty = 1;
		return 0;
	} else if (b == -1) {
		int fd;
		info("Creating a new, blank cull_atimes file.");
		/* Only the cull_index file exists -- create a new, blank atimes file.
		 * Allow the code below to extend the size of this file as needed. */
		fd = creat(s->atimefile, (S_IRUSR|S_IWUSR));
		ret_syserr((fd < 0), "cull_atimes is missing, but I was unable to create a new one");
		close(fd);
	}

	return 0;
}


/**
 * check_fsizes will ensure that both indices have reasonable sizes,
 * the number of records they describe match one another,
 * and compute cache state like 'perpage,' 'num_indices' and so on.
 * @param s The cache state to fill out.
 * @return An errno-compatible integer.
 */
static int check_fsizes(cstate *s)
{
	struct stat st;
	int rc;
	size_t trunc;

	/* Retrieve stats from the index file */
	do {
		rc = stat(s->indexfile, &st);
		ret_syserr((rc != 0), "Failed to stat() index file");

		/* compute the number of entities per page. */
		s->index_size = st.st_size;
		s->num_perpage = (s->pagesize / s->ent_size);
		s->num_indices = (st.st_size / s->pagesize) * s->num_perpage;

		/* Verify that the index file is sized reasonably. */
		if (!(s->index_size % s->pagesize)) break;

		s->need_fsck = 1;
		warning("Issue: index (%lu) not a multiple of the pagesize.",
			s->index_size);

		rc = truncate(s->indexfile, mult_ceil(s->index_size, s->pagesize));
		ret_syserr((rc != 0), "Failed to extend the index to be a multiple of the pagesize.");

	} while (s->index_size % s->pagesize);

	/* Investigate the atimes file. */
	do {
		/* Retrieve stats from the atimes file. */
		rc = stat(s->atimefile, &st);
		ret_syserr((rc != 0), "Failed to stat atimes file");

		s->atime_size = st.st_size;
		s->num_atimes = (st.st_size / sizeof(atime_t));

		/* Ensure the atimes file is sized reasonably. */
		if (!(s->atime_size % sizeof(atime_t)) &&
		    s->num_indices == s->num_atimes) break;

		s->need_fsck = 1;
		warning("Issue: atimes filesize is not a multiple of (%lu * %u).\n",
			sizeof(atime_t), s->num_perpage);

		/* Number of indices times size of atime,
		 * rounded up to the nearest entities-per-page boundary. */
		trunc = mult_ceil(s->num_indices * sizeof(atime_t),
				  s->num_perpage * sizeof(atime_t));
		rc = truncate(s->atimefile, trunc);
		ret_syserr((rc != 0), "Failed to extend the atimes file to be a multiple of %lu",
			   sizeof(atime_t) * s->num_perpage);

	} while ((s->atime_size % sizeof(atime_t)) ||
		 (s->num_indices != s->num_atimes));

	return 0;
}


/**
 * load_page will load the appropriate pages of data from the indices into
 * the state buffers for further inspection.
 * @param s The cache state to use to load the pages.
 * @return An errno-compatible integer.
 */
static int load_page(cstate *s)
{
	struct scan_state *t = scanstate(s);
	int rc;

	if ((rc = check_scan_state(s)))
		goto error;

	if (!t->indices_open) {
		rc = open_indices(s);
		jmp_usrerr((rc != 0), error,
			   "Error opening indices on-demand in load_page.");
	}

	/* Jump to the appropriate loading method ... */
	rc = _load_page(s);
	jmp_usrerr((rc != 0), error, "");

	/* Clang workaround: clang won't follow _load_page ... */
	if (!(t->buffer && t->abuffer))
		error("load_page failure: buffers are unallocated/unmapped");

	t->page_loaded = 1;
	t->dirty = 0;
	t->loads++;
	debug(2, "--- (%u) Read page #%lu. ---", getpid(), t->pageno);
	return 0;
error:
	return rc;
}


#ifndef BUFFER
static int _load_page(cstate *s)
{
	int rc;
	long offset, length;
	struct scan_state *t = scanstate(s);

	/* Map the index buffer. Straightforward. */
	t->buffer = mmap(NULL, s->ent_size * s->num_perpage,
			 PROT_READ | PROT_WRITE, MAP_SHARED, t->index_fd,
			 t->pageno * s->pagesize);
	ret_syserr((t->buffer == MAP_FAILED),
		   "Failed to map page for Index file.");


	/* Since maps have to align to page boundaries,
	 * find out how low we have to go, record the difference. */
	offset = t->pageno * s->num_perpage * sizeof(atime_t);
	length = sizeof(atime_t) * s->num_perpage;
	t->map_delta = offset - mult_floor(offset, s->pagesize);
	debug(4, "mmap delta to nearest page: %lu", t->map_delta);

	/* Map the atimes buffer, then adjust the pointer. */
	t->abuffer = mmap(NULL, length + t->map_delta, PROT_READ | PROT_WRITE,
			  MAP_SHARED, t->atime_fd, offset - t->map_delta);
	jmp_syserr((t->abuffer == MAP_FAILED), unwind,
		   "Failed to map page for Atimes file.");
	/* Make buffer[0] correspond to abuffer[0]. */
	t->abuffer += t->map_delta / sizeof(atime_t);

	return 0;
unwind:
	munmap(t->buffer, t->pageno * s->pagesize);
	return rc;
}
#endif

#ifdef BUFFER
static int _load_page(cstate *s)
{
	struct scan_state *t = scanstate(s);
	int rc;
	long offset, items;

	offset = (t->pageno * s->pagesize);
	rc = fseek(t->indexfh, offset, SEEK_SET);
	jmp_syserr((rc != 0), error, "Failed to seek to [%ld] in [%s]",
		   offset, s->indexfile);

	offset = (t->pageno * s->num_perpage * sizeof(atime_t));
	rc = fseek(t->atimefh, offset, SEEK_SET);
	jmp_syserr((rc != 0), error, "Failed to seek to [%ld] in [%s]",
		   offset, s->atimefile);

	items = fread(t->abuffer, sizeof(atime_t),
		      s->num_perpage, t->atimefh);
	jmp_syserr((items != s->num_perpage), error,
		   "Failed to read a page's worht of atimes.");

	items = fread(t->buffer, s->ent_size,
		      s->num_perpage, t->indexfh);
	jmp_syserr((items != s->num_perpage), error,
		   "Failed to read a page of index record entries");

	return 0;
error:
	return rc;
}
#endif


/**
 * save_page recommits the current buffered pages back to disk.
 * (If maps are being used, it will unmap them.)
 *
 * @param s The cachefilesd_state containing the buffered pages
 * @return An errno-compatible integer.
 */
static int save_page(cstate *s)
{
	int rc;
	struct scan_state *t = scanstate(s);

	if ((rc = check_scan_state(s))) return rc;

	rc = _save_page(s);
	jmp_usrerr((rc != 0), error, "");

	t->dirty = 0;
	return 0;
error:
	return rc;
}


#ifdef BUFFER
/* If we use buffering instead of maps. */
static int _save_page(cstate *s)
{
	int rc;
	long offset;
	size_t items;
	struct scan_state *t = scanstate(s);

	if ((rc = check_scan_state(s))) return rc;
	/* Nothing to do. */
	if (!t->dirty) return 0;

	debug(1, "Page is dirty, recommitting to disk; page:%lu offset:%lu",
	      t->pageno, s->pagesize * t->pageno);

	offset = s->pagesize * t->pageno;
	rc = fseek(t->indexfh, offset, SEEK_SET);
	jmp_syserr((rc != 0), error, "Failed to seek to [%ld] in [%s]",
		   offset, s->indexfile);

	offset = t->pageno * s->num_perpage * sizeof(atime_t);
	rc = fseek(t->atimefh, offset, SEEK_SET);
	jmp_syserr((rc != 0), error, "Failed to seek to [%ld] in [%s]",
		   offset, s->atimefile);

	items = fwrite(t->buffer, s->ent_size, s->num_perpage, t->indexfh);
	jmp_syserr((items != s->num_perpage), error,
		   "Failed to recommit dirty index page back to disk");

	items = fwrite(t->abuffer, sizeof(atime_t), s->num_perpage, t->atimefh);
	if (items != s->num_perpage) {
		rc = errno;
		dperror("Failed to recommit dirty atime page back to disk");
		debug(0,"*** WARNING ***: Recommitted index, but not atimes.\n"
		      "Indices may now be out of sync. Re-run an index check.");
		goto error;
	}

	return 0;
error:
	return rc;
}
#else


static int _save_page(cstate *s)
{
	int rc;
	struct scan_state *t = scanstate(s);

	if (t->buffer) {
		rc = munmap(t->buffer, s->ent_size * s->num_perpage);
		jmp_syserr((rc == -1), error, "Index munmap failed");
		t->buffer = NULL;
	}

	if (t->abuffer) {
		ssize_t length = sizeof(atime_t) * s->num_perpage;
		rc = munmap(t->abuffer - t->map_delta / sizeof(atime_t), length + t->map_delta);
		jmp_syserr((rc == -1), error, "Atimes munmap failed");
		t->abuffer = NULL;
	}

	t->map_delta = 0;
	/* technically, save_page for the buffer method never "unloads" a page,
	 * it only syncs the buffers and disk, so we never unset this for that method. */
	t->page_loaded = 0;
	return 0;
error:
	return rc;
}
#endif


/**
 * Tests if the given record entity is "empty" or not.
 * @param r The record to test.
 * @return False if occupied, True if empty.
 */
static inline bool is_empty(const struct index_record *r)
{
	/* We count an empty type OR length
	 * to imply that the slot is empty. */
	return (!r->type || !r->len);
}

/**
 * Tests if a given slot is definitely actually empty.
 * @param t The scan state to investigate.
 * @return true if it's empty, false if it's inconsistent or occupied.
 */
static inline bool is_real_empty(const struct scan_state *t)
{
	return (is_empty(t->r) && !(t->abuffer[t->local_index]));
}


/**
 * A brief function that checks for default dir entries.
 * @param d_name The name of the directory entry.
 * @return True if the fiename is "." or "..", False otherwise.
 */
static inline bool is_dotdir(const char *d_name)
{
	if (d_name[0] == '.') {
		if (!d_name[1] || (d_name[1] == '.' && !d_name[2])) {
			return true;
		}
	}
	return false;
}


/**
 * A brief function that checks for recognized file types.
 *
 * @param d_type The dirent.d_type field.
 * @return True if we recognize it (DIR, REG or UNKNOWN),
 * False if we do not.
 */
static inline bool is_goodtype(int d_type)
{
	switch (d_type) {
	case DT_UNKNOWN:
	case DT_DIR:
	case DT_REG:
		return true;
	default:
		return false;
	}
}


/**
 * A brief predicate function for determining if
 * the name of a file matches its filetype.
 *
 * @param d_name The name of the file.
 * @param mode The mode of the file, as retrieved from stat64.
 *
 * @return False if the file is incorrectly named
 * with respect to its type.
 */
static bool is_expected(const char *d_name, mode_t mode)
{
	if (memchr("IDSJET+@", d_name[0], 8) == NULL) {
		return false;
	}

	if (!S_ISDIR(mode) &&
	    (!S_ISREG(mode) ||
	     (memchr("IJ@+", d_name[0], 4) != NULL)))
		return false;

	return true;
}


/**
 * get_fd_at is a convenient wrapper for open_by_handle_at.
 * We utilize the file handle stored in the index record
 * to obtain a file handle relative to the root dir passed in.
 *
 * @param root A directory expected to be an ancestor of the cache object.
 * @param rec The index record data, including the encoded file handle.
 *
 * @return A valid file descriptor, or -1 on error (with errno set.)
 */
static int get_fd_at(const char *root, const struct index_record *rec)
{
	int retval, dirfd, rc = 0;
	struct file_handle *fh;

	if (!rec || !root) {
		errno = EINVAL;
		retval = -1;
		goto leave;
	}

	/* Copy our record into a format that syscalls will understand:
	 * the two structures are padded differently. */
	fh = malloc(sizeof(struct file_handle) + rec->len);
	if (!fh) {
		rc = errno;
		dperror("Failed to allocate memory for filehandle");
		retval = -1;
		goto leave;
	}
	fh->handle_bytes = rec->len;
	fh->handle_type = rec->type;
	memcpy(fh->f_handle, rec->fh, rec->len);

	/* Open the root cache dir */
	dirfd = open(root, O_DIRECTORY);
	if (dirfd < 0) {
		rc = errno;
		dperror("Failed to open root directory [%s]", root);
		retval = -1;
		goto free1;
	}

	/* Open the file referenced. */
	retval = open_by_handle_at(dirfd, fh, 0);
	if (retval < 0) {
		rc = errno;
		dperror("open_by_handle_at failed");
		retval = -1;
		goto close1;
	}

close1:
	if (close(dirfd)) {
		/* Something goofed up badly */
		oserror("Could not close directory: [%s]", root);
	}
free1:
	free(fh);
leave:
	errno = rc;
	return retval;
}


/**
 * name_to_handle_at helper utility.
 * @param dirfd Where to convert to a handle at.
 * @param name The name of the file to get a handle for.
 * @return NULL on failure, check errno for details.
 * @sideeffect On success, a new memory allocation pointer is returned,
 * it is up to the user to free it.
 */
struct file_handle *ntha(int dirfd, const char *name)
{
	int mnt_id, rc;
	struct file_handle *fh;
	const size_t size = MAX_HANDLE_SZ;
	/* NB: get_handle(...), below, assumes size = MAX_HANDLE_SZ. */

	fh = malloc(sizeof(struct file_handle) + size);
	if (!fh) {
		rc = errno;
		dperror("Failed to allocate memory for file_handle");
		errno = rc;
		return NULL;
	}
	fh->handle_bytes = size;

	if (name_to_handle_at(dirfd, name, fh, &mnt_id, 0) != 0) {
		rc = errno;
		dperror("Failed to convert [%s] to a file_handle", name);
		free(fh);
		errno = rc;
		return NULL;
	}

	return fh;
}


/**
 * get_handle is a wrapper around ntha that will get an object's handle,
 * as well as its parents, conjoined into a single entry.
 * this mimics the behavior of exportfs_encode_fh.
 * @param dirfd An open file descriptor to the directory this file lives within.
 * @param name What file should we retrieve a handle for?
 * @param isdir Is this a directory?
 * @param[out] return_fh A handle to the item on success. Unchanged otherwise.
 * @return An errno-compatible integer.
 * @sideeffect Allocates memory for the file handle and saves it to *return_fh.
 */
int get_handle(int dirfd, const char *name, bool isdir,
	       fht **return_fh)
{
	int rc;
	struct file_handle *fh, *pfh, *temp;

	/* Get the object's file_handle. */
	fh = ntha(dirfd, name);
	if (!fh)
		return errno;
	if (isdir)
		goto success;

	/* If the object is not a directory, get his parent's as well. */
	pfh = ntha(dirfd, ".");
	if (!pfh) {
		rc = errno;
		goto free_fh;
	}

	/* Extend fh if needed to fit both results. */
	/* NB: handle_bytes is updated to the "real" length of the handle,
	 * Although NTHA allocates MAX_HANDLE_SZ for it. */
	if (fh->handle_bytes + pfh->handle_bytes > MAX_HANDLE_SZ) {
		temp = realloc(fh, sizeof(struct file_handle) +
			       fh->handle_bytes + pfh->handle_bytes);
		jmp_syserr((temp == NULL), free_both,
			   "Failed to allocate space to hold combined file_handle");
		fh = temp;
	}

	/* Join the results. */
	memcpy(&(fh->f_handle[fh->handle_bytes]),
	       &(pfh->f_handle[0]),
	       pfh->handle_bytes);
	fh->handle_bytes += pfh->handle_bytes;

	/* Free the parent's filehandle */
	free(pfh);

success:
	*return_fh = fh;
	return 0;

free_both:
	free(pfh);
free_fh:
	free(fh);
	return rc;
}


/**
 * Given a filename, obtain this file's culling index slot number, if any.
 * @param filename The file to investigate.
 * @param[out] slot Return value, the slot number if any.
 * @return An errno-compatible integer.
 */
int read_slot(const char *filename, slot_t *slot)
{
	int fd, rc;
	fd = open(filename, O_RDONLY);
	ret_syserr((fd < 0), "Error opening [%s] for reading xattrs", filename);

	rc = read_slot_fd(fd, slot);
	jmp_usrerr((rc != 0 && rc != ENOATTR), close, "Couldn't read slot xattrs.");

close:
	close(fd);
	return rc;
}


/**
 * Given a file descriptor, obtain this file's
 * culling index slot number, if any.
 * @param fd File descriptor of file.
 * @param[out] slot Return value: the slot number, if any.
 * @return An errno-compatible integer.
 */
int read_slot_fd(int fd, slot_t *slot)
{
	struct cache_xattr_usr *x = NULL;
	int rc;

	rc = _bx(fd, slot_xattr, (struct generic_xattr **)&x);
	if (rc == ENOATTR) goto err;
	else jmp_usrerr((rc != 0), err, "Failed to read xattrs");

	if (x->xattr.len < sizeof(struct cache_xattr)) {
		/* file has xattrs, but they don't contain slot info. */
		*slot = CACHEFILES_NO_CULL_SLOT;
		rc = ENOATTR;
	} else {
		*slot = x->cachedata.cullslot;
		rc = 0;
	}

	free(x);
	return rc;
err:
	*slot = CACHEFILES_NO_CULL_SLOT;
	return rc;
}


/**
 * record_seek sets s->scan->r, the index_record pointer,
 * to a valid entry representing the slot requested.
 * This is mostly useful for out-of-order scanning.
 * r is otherwise manipulated directly in scan_page.
 *
 * @param s The state to adjust so that it references slot_no
 * @param slot_no The slot to bring into focus.
 * @return An errno-compatible integer.
 */
static int record_seek(cstate *s, slot_t slot_no)
{
	int rc;
	struct scan_state *t = scanstate(s);
	size_t pageno, local;

	if ((rc = check_scan_state(s)))
		return rc;

	pageno = fpageno(slot_no, s->num_perpage);
	local = slot_no % s->num_perpage;

	/* Bring up the right page, if needed: */
	rc = page_seek(s, pageno);
	ret_usrerr((rc != 0), "record_seek: failed to call page_seek.");

	t->index = slot_no;
	t->local_index = local;
	t->r = get_record(s);

	return 0;
}


/**
 * page_seek is effectively an on-demand page save/load mechanism.
 * If the page requested is different from what is already loaded,
 * The page will be saved/closed and the new page opened.
 * @param s The cache state.
 * @param pageno The page to load.
 * @return An errno-compatible integer.
 */
static int page_seek(cstate *s, size_t pageno)
{
	int rc;
	struct scan_state *t = scanstate(s);

	if ((rc = check_scan_state(s)))
		return rc;

	if ((pageno != t->pageno) || (!t->page_loaded)) {
		rc = save_page(s);
		ret_usrerr((rc != 0), "page_seek: error saving page.");

		t->pageno = pageno;

		rc = load_page(s);
		ret_usrerr((rc != 0), "page_seek: error loading page.");
	}

	return 0;
}


/******************************************************************************/
/* Remedial Actions							      */
/******************************************************************************/

/**
 * Mark a slot as empty. Will perform the action
 * locally without the assistance of the kernel
 * if being run in "scan only" mode.
 *
 * @param s The cache to delete the slot from.
 * @param slot_no Which slot to delete.
 *
 * @return An errno-compatible integer.
 */
static int delete_slot(cstate *s, slot_t slot_no)
{
	int rc = check_state(s);
	if (rc) return rc;

	rc = s->bound ?
		_delete_slot_online(slot_no) :
		_delete_slot_offline(s, slot_no);

	if (!rc && s->scan) s->scan->fixes++;
	return rc;
}


/**
 * repair_slot will adjust the xattr values of the matching file
 * such that the file the index points to now points back at that index.
 * It is somewhat a misnomer, since we are changing the file, not the slot.
 *
 * @param s The cachefilesd cache state structure.
 * @param slot_no The slot to fix.
 * @param x An xattr structure describing the file and his xattrs,
 * Can be NULL if s->bound is true (When the kernel is bound to the cache.)
 *
 * @return An errno-compatible integer.
 */
static int repair_slot(cstate *s, slot_t slot_no, struct cache_xattr_usr *x)
{
	int rc = check_state(s);
	if (rc) return rc;

	rc = s->bound ?
		_repair_slot_online(slot_no) :
		_repair_slot_offline(slot_no, x);

	if (!rc && s->scan) s->scan->fixes++;
	return rc;
}


/**
 * delete_file is a remedial action that deletes a file.
 * This function is intended to be called on files verified
 * to be orphans -- verified to have NO index entry whatsoever.
 * @param s The cache to delete the file from.
 * @param dirfd A file descriptor to the directory we're deleting from.
 * @param name The name of the doomed file.
 *
 * @return An errno-compatible integer.
 */
int delete_file(cstate *s, int dirfd, const char *name)
{
	int rc;

	rc = s->bound ?
		_delete_file_online(name) :
		_delete_file_offline(dirfd, name, NULL);

	if (!rc && s->scan) s->scan->fixes++;
	return rc;
}


/**
 * delete_dir removes an empty directory,
 * maintaining consistency if it has an xattr slot number.
 * @param s The cachefilesd state.
 * @param dirname The directory to delete.
 *
 * @return An errno-compatible integer, possibly ENOTEMPTY.
 */
static int delete_dir(cstate *s, const char *dirname)
{
	int rc;

	if (s->bound) {
		/* Paranoia: although this is only called on directories
		 * we suspect to be empty, double-check... */
		if (!empty_dir(dirname)) {
			return ENOTEMPTY;
		}
		/* Paranoia: piggyback on _delete_file_online,
		 * which will refuse to delete a non-empty directory
		 * that is found to be a valid cache object. */
		rc = _delete_file_online(dirname);
		jmp_usrerr((rc != 0), leave, "Failed to delete_dir(%s).", dirname);

	} else {
		/* otherwise, we have to take some extra care,
		 * because we need to maintain index consistency --
		 * destroy_file is inappropriate here. */
		rc = _delete_dir_offline(s, dirname);
	}

leave:
	if (!rc && s->scan) s->scan->fixes++;
	return rc;
}

/******************************************************************************/
/* Remedial Implementations						      */
/******************************************************************************/

/**
 * _delete_slot_offline is the implementation for removing
 * a slot from both indices when the kernel module is not bound.
 *
 * @param s The cache to delete from.
 * @param slot_no Which slot to delete.
 *
 * @return An errno-compatible integer.
 */
static int _delete_slot_offline(cstate *s, slot_t slot_no)
{
	int rc;
	struct scan_state *t = scanstate(s);

	rc = check_scan_state(s);
	if (rc) return rc;

	/* Bring the record up (/if needed/) */
	rc = record_seek(s, slot_no);
	ret_usrerr((rc != 0), "Failed to bring record up for deletion.");

	/* Set the type, length and atime to zero. */
	t->r->type = 0x0;
	t->r->len = 0x0;
	t->abuffer[t->local_index] = 0;
	t->dirty = 1;

	return 0;
}


/**
 * Delete a slot by invoking the kernel module interface.
 *
 * @param slot_no The slot to delete.
 * @return An errno-compatible integer.
 */
static int _delete_slot_online(slot_t slot_no)
{
	char sbuff[20];
	int len, ret, rc;

	len = snprintf(sbuff, 20, "rmslot %u", slot_no);
	ret = write(cachefd, sbuff, len);
	ret_syserr((ret < len),
		   "Error sending command: [%s], (written %d < len %d)",
		   sbuff, ret, len);

	return 0;
}


/**
 * _repair_slot_offline modifies a file's slot number xattr such that
 * it matches the given slot number again.
 * @param slot_no The slot number to update the file to.
 * @param x xattribute structure, describing the file to repair and its current xattr value.
 * @return An errno-compatible integer.
 */
static int _repair_slot_offline(slot_t slot_no, struct cache_xattr_usr *x)
{
	int rc;
	if (x == NULL) return EINVAL;

	x->cachedata.cullslot = slot_no;

	rc = _sx(x->xattr.fd, slot_xattr, x->xattr.xdata, x->xattr.len);
	ret_usrerr((rc != 0), "Failed to repair slot information on file.");

	return 0;
}


/**
 * _repair_slot_online sends a fixslot nnn command to the kernel,
 * which will update a file's xattr to match the specified value.
 * @param slot_no the slot to instruct the kernel to repair.
 * @return An errno-compatible integer.
 */
static int _repair_slot_online(slot_t slot_no)
{
	char sbuff[20];
	int len, ret, rc;

	len = snprintf(sbuff, 20, "fixslot %u", slot_no);
	ret = write(cachefd, sbuff, len);
	ret_syserr((ret < len),
		   "sent [%s], strlen was %d, received back %d",
		   sbuff, len, ret);

	return 0;
}


/**
 * _delete_file_offline will obliterate a file or
 * directory without kernel assistance.
 * In the case of directories, the folder will be moved to the
 * graveyard. In this case it is important that children are
 * verified to be invalid or otherwise safe to delete,
 * because no consistency checking will be performed.
 * @param dirfd The dirfd to the containing directory
 * @param name The file to obliterate.
 * @param st (Optional) A stat structure describing the file.
 * @return An errno-compatible integer.
 */
static int _delete_file_offline(int dirfd, const char *name,
				const struct stat64 *st)
{
	int rc;
	bool isdir;

	/* de->d_type is unreliable. fetch mode using stat() if needed. */
	if (st == NULL) {
		struct stat64 st_local;
		fstatat64(dirfd, name, &st_local, 0);
		isdir = S_ISDIR(st_local.st_mode);
	} else {
		isdir = S_ISDIR(st->st_mode);
	}

	if (!isdir) {
		rc = unlinkat(dirfd, name, 0);
		ret_syserr((rc < 0 && errno != ENOENT), "Unable to unlink file: %s", name);
	} else {
		char namebuf[40];
		static unsigned uniquifier;
		struct timeval tv;
		gettimeofday(&tv, NULL);
		sprintf(namebuf, "x%lxx%xx", tv.tv_sec, uniquifier++);

		rc = renameat(dirfd, name, graveyardfd, namebuf);
		ret_syserr((rc < 0 && errno != ENOENT),
			   "Unable to rename file [%s]-->[%s]", name, namebuf);
	}

	return 0;
}


/**
 * _delete_file_online utilizes the kernel to safely cull a file.
 *
 * delete_file is called:
 * 1) on files with a bad name/type combo (e.g, a dir named D... or a file named +...)
 * 2) When fsck_file returns EEXIST
 *    a) When the file has no xattrs
 *    b) When the file has an out of bounds cullslot (but not PINNED)
 *    c) When the file points to a slot with the wrong filehandle**
 *	 **We assume this to mean this file is an orphan, because all index
 *				    entries were repaired in the table scan.
 *
 * In all cases, we find it appropriate to use the CULL command.
 * For orphaned objects, cull will correctly identify via validate_slot that
 *		 the object is bad and will not delete entries in the index.
 * Out of bounds objects are intercepted by cachefiles_cx_validate_slot and are assumed stale.
 * For objects without xattrs, it will assume it has a stale slot and delete it.
 *
 * @param filename The name of the file to delete.
 * @return An errno-compatible integer.
 */
static int _delete_file_online(const char *filename)
{
	int rc, len;
	char cmdbuff[NAME_MAX + 30];

	len = snprintf(cmdbuff, NAME_MAX + 30, "cull %s", filename);
	if (len >= NAME_MAX + 30) {
		rc = ENAMETOOLONG;
		debug(0, "Error preparing cull command for file [%s]",
		      filename);
		return rc;
	}

	rc = write(cachefd, cmdbuff, len);
	ret_syserr((rc < len), "Failed to send command: %s", cmdbuff);

	return 0;
}


/**
 * Attempt to remove a directory while not bound to the cache.
 * More than just invoking rmdir, we need to make sure that
 * if the directory is an index, we clean up the cull_index.
 *
 * @param s The cachefilesd state.
 * @param dirname The directory to delete.
 * @return An errno-compatible integer.
 * We may silently opt not to delete
 * a file that isn't empty or not a directory,
 * in this case, the return code is 0 but a more
 * descriptive error is stored to errno.
 */
static int _delete_dir_offline(cstate *s, const char *dirname)
{
	int rc;
	slot_t slot;
	bool has_slot = false;

	/* See if he has a slot, and record what it is. */
	rc = read_slot(dirname, &slot);
	if (rc && rc != ENOATTR) {
		debug(0, "Error determining slot information for [%s]", dirname);
		return rc;
	}
	else if (rc == ENOATTR) has_slot = false;

	/* Attempt to rmdir the directory. */
	rc = rmdir(dirname);
	if (rc == 0) {
		if (!has_slot) {
			errno = ENOATTR;
			return 0;
		}
		return _delete_slot_offline(s, slot);
	}

	switch((rc = errno)) {
	case ENOTDIR:
	case ENOENT:
	case ENOTEMPTY:
		/* soft error: not a big deal. */
		debug(2, "Attempted to rmdir(%s), but: %m", dirname);
		errno = rc;
		return 0;
	default:
		/* This is a problem, though. */
		dperror("Failed to remove directory: [%s]", dirname);
		return rc;
	}
}

/******************************************************************************/
/* Utility.								      */
/******************************************************************************/

/**
 * Return a newly allocated buffer that concatenates a and b.
 * @param a First half of the string.
 * @param b The second half.
 * @return a newly allocated buffer. Remember to free it.
 * Will return NULL on failure, and errno set appropriately.
 * @sideeffect Allocates memory that must be freed if the function succeeds.
 */
char *str2cat(const char *a, const char *b)
{
	int rc;
	char *newstring = calloc(strlen(a) + strlen(b) + 1, 1);

	if (!newstring) {
		rc = errno;
		dperror("Failed to allocate memory for string");
		errno = rc;
		return NULL;
	}

	strcat(newstring, a);
	strcat(newstring, b);

	return newstring;
}


/**
 * Return the ceiling of n, rounded up to the nearest multiple.
 * @param n Input
 * @param multiple The integer multiple to round with.
 * @return n, rounded up to the nearest (N*multiple).
 */
__attribute__ ((pure)) static inline size_t mult_ceil(size_t n, size_t multiple)
{
	return multiple * ((n + multiple - 1) / multiple);
}


/**
 * Return the floor of n, rounded down to the nearest multiple.
 * @param n Input.
 * @param multiple The integer multiple to round with.
 * @return n, rounded down to the nearest (N*multiple).
 */
__attribute__ ((pure)) static inline size_t mult_floor(size_t n, size_t multiple)
{
	return multiple * (n / multiple);
}


/**
 * Verify that the state is usable (non-null) and initialized.
 * @param s The state to verify.
 * @return An errno-compatible integer.
 */
static inline int check_state(const cstate *s)
{
	if (!s) return EINVAL;
	if (!s->init) return EINVAL;
	return 0;
}


/**
 * Verify that the state is usable AND has valid scanning (fsck) state.
 * @param s The state to verify.
 * @return An errno-compatible integer.
 */
static inline int check_scan_state(const cstate *s)
{
	int rc = check_state(s);
	if (rc)
		return rc;
	if (!s->scan)
		return EINVAL;
	return 0;
}


/**
 * Print out the current index record of interest.
 * @param t The scan status to use.
 */
static void print_record(const struct scan_state *t)
{
	/* Debug print-info of this record. */
	unsigned short k;
	const struct index_record *r = t->r;

	debug(3, "* index: %08u; lindex: %08u; buf: %p; abuf: %p",
	      t->index, t->local_index, t->buffer, t->abuffer);
	debug(2, "index: %08u; atime: %08u; type: %02x;"
	      "len: %02x; handle: 0x",
	      t->index, t->abuffer[t->local_index], r->type, r->len);

	/* Loop through the bytes in the record. */
	for (k = 0; k < r->len; ++k) {
		debug_nocr(2, "%02x", r->fh[k]);
	}
	debug_nocr(2, "\n");
}

/**
 * Print an encoded file handle.
 * @param fh The handle to print.
 */
static void print_handle(const struct file_handle *fh)
{
	unsigned i;
	debug_nocr(2, "\thandle: ");
	for (i = 0; i < fh->handle_bytes; ++i) {
		debug_nocr(2, "%02x", fh->f_handle[i]);
	}
	debug_nocr(2, "\n");
}


/**
 * Simple routine to print an xattr as a string.
 * @param x The xattribute structure to print.
 */
static void print_xattr(const struct cache_xattr_usr *x)
{
	unsigned i;

	for (i = 0; i < x->xattr.len; ++i)
		debug_nocr(4, "%02x", x->xattr.xdata[i]);
	debug_nocr(4, "\n");
}


/**
 * Tells you if a directory is empty or not.
 * @param dirname The directory to investigate.
 * @return 1 if it is indeed empty.
 * 0 either indicates it is non-empty, or an error occurred.
 * investigate errno for further information.
 */
static bool empty_dir(const char *dirname)
{
	DIR *dh;
	bool empty = false;

	dh = opendir(dirname);
	if (!dh)
		return false;

	/* empty_dir_dh returns !empty and sets errno on failure ... */
	empty = empty_dir_dh(dh);

	if (closedir(dh))
		oserror("Failure closing directory handle");

	return empty;
}


/**
 * Tells you if a directory is empty or not.
 * @param dh An opened DIR* handle to a directory.
 * @return 1 if it is indeed empty.
 * 0 either indicates it is non-empty, or an error occurred.
 * investigate errno for further information.
 */
static bool empty_dir_dh(DIR *dh)
{
	int rc;
	struct dirent entry, *de;
	bool empty = false;
	long dir_pos;

	if (!dh) {
		errno = EINVAL;
		return false;
	}

	dir_pos = telldir(dh);
	if (dir_pos < 0) return false;

	rewinddir(dh);

	while(1) {
		/* readdir_r returns an errno as a retcode */
		rc = readdir_r(dh, &entry, &de);
		if (rc)
			goto leave;
		if (de == NULL) {
			empty = true;
			break;
		}
		if (is_dotdir(entry.d_name))
			continue;
		/* Found something here. */
		break;
	}

	/* success! */
	rc = 0;

leave:
	seekdir(dh, dir_pos);
	errno = rc;
	return empty;
}


/**
 * Based on the current local_index, retrieve a pointer from the scan buffer
 * for the current index_record of interest.
 * @param s The cache state.
 * @return The index_record* as computed from t->local_index, from within scan->buffer[].
 */
static inline struct index_record *get_record(const cstate *s)
{
	return (struct index_record *) &(s->scan->buffer[s->ent_size * s->scan->local_index]);
}


/**
 * Safely retrieve an index record pointer, if any.
 * @param t The scan_state to retrieve the record from.
 * @return The index_record*, or NULL.
 */
static inline struct index_record *record(const struct scan_state *t)
{
	return t ? t->r : NULL;
}


/**
 * Safely retrieve a scan state pointer, if any.
 * @param s The cache state.
 * @return the scan_state*, or NULL.
 */
static inline struct scan_state *scanstate(const cstate *s)
{
	return s ? s->scan : NULL;
}
