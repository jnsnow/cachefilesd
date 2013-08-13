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

/** Used to pass state to the SIGCHLD handler.
 * If the daemon is modified to support multiple caches,
 * this will likely need to be replaced with a (PID, state*) map.
 */
struct cachefilesd_state *fork_state = NULL;

typedef struct file_handle fhandle_t;

/* Function declarations-only */
static char	  *str2cat     (const char *a, const char *b);
static scan_rc	   delete_slot (struct index_record *r, struct scan_state *t, int bound);
static scan_rc	   repair_slot (struct scan_state *t, struct xattr_repair *xr, int bound);
static int	   delete_file (int dirfd, struct dirent *de, int bound);
static int	   read_state  (struct cachefilesd_state *s);
static int	   is_expected (char *d_name, struct stat64 *st);
static fhandle_t  *ntha	       (int dirfd, const char *name, size_t size);
static int	   scan_file   (struct cachefilesd_state *s, DIR *dh, struct dirent *de, struct stat64 *st);
static int	   spider      (struct cachefilesd_state *s, const char *relpath, int *num);
static scan_rc	   scan_slot   (struct cachefilesd_state *s, struct scan_state *t);
static scan_rc	   scan_page   (struct cachefilesd_state *s, struct scan_state *t);
static int	   scan_table  (struct cachefilesd_state *s);


/******************************************************************************/


/**
 * Return a newly allocated buffer that concatenates a and b.
 * @param a First half of the string.
 * @param b The second half.
 * @return a newly allocated buffer. Remember to free it.
 * Will return NULL on failure.
 */
char *str2cat(const char *a, const char *b)
{
	char *newstring = calloc(strlen(a) + strlen(b) + 1, 1);

	if (!newstring) {
		dperror("Failed to allocate memory for string");
		return NULL;
	}

	strcat(newstring, a);
	strcat(newstring, b);

	return newstring;
}


/**
 * Tests if the given record entity is "empty" or not.
 * @param r The record to test.
 * @return 0 if occupied, 1 if free/empty.
 */
static inline int is_empty(struct index_record *r)
{
	/* We count an empty type OR length
	 * to imply that the slot is empty. */
	return (!r->type || !r->len);
}


/**
 * A brief function that checks for default dir entries.
 * @param d_name The name of the directory entry.
 * @return 1 if it's . or .., 0 otherwise.
 */
static inline int is_dotdir(char *d_name)
{
	if (d_name[0] == '.') {
		if (!d_name[1] || (d_name[1] == '.' && !d_name[2])) {
			return 1;
		}
	}
	return 0;
}


/**
 * A brief function that checks for recognized file types.
 * @param d_type The dirent.d_type field.
 * @return 1 if we recognize it (DIR, REG or UNKNOWN),
 * 0 if we do not.
 */
static inline int is_goodtype(int d_type)
{
	switch (d_type) {
	case DT_UNKNOWN:
	case DT_DIR:
	case DT_REG:
		return 1;
	default:
		return 0;
	}
}


/**
 * Mark a slot as empty. Will perform the action
 * locally without the assistance of the kernel
 * if being run in "scan only" mode.
 * @param r The record to delete.
 * @param t The scan state to delete with/from
 * @param bound a boolean: are we bound to the cache already?
 *
 * @return
 * E_ERROR if the kernel gave us an error,
 * E_OK	   if the kernel handled it for us,
 * E_DIRTY if we did it ourselves and
 * the page this slot belongs to needs to be recommitted.
 */
scan_rc delete_slot(struct index_record *r,
		    struct scan_state *t,
		    int bound)
{
	if (bound) {
		char sbuff[20];
		int len, ret;

		len = snprintf(sbuff, 20, "rmslot %u", t->index);
		ret = write(cachefd, sbuff, len);
		if (ret < len) {
			debug(0, "Error sending command: [%s] (%d < %d)", sbuff,
			      ret, len);
			return E_ERROR;
		}
		return E_OK;
	} else {
		/* Set the type, length and atime to zero. */
		r->type = 0x0;
		r->len = 0x0;
		t->abuffer[t->local_index] = 0;
		return E_DIRTY;
	}
}


/**
 * repair_slot will adjust the xattr values
 * so that the file the index points to
 * has the correct index number.
 *
 * @param t The scan state structure, housing info about this record,
 * @param xr An xattr_repair structure describing the file and his xattrs.
 * @return
 * E_OK if everything went OK,
 * E_ERROR if we encountered an unrecoverable error.
 */
scan_rc repair_slot(struct scan_state *t, struct xattr_repair *xr, int bound)
{
	int rc = E_OK;

	if (bound) {
		char sbuff[20];
		int len, ret;
		len = snprintf(sbuff, 20, "fixslot %u", t->index);
		ret = write(cachefd, sbuff, len);
		if (ret < len) {
			debug(0, "sent [%s], strlen was %d, received back %d",
			      sbuff, len, ret);
			dperror("errno says");
			rc = E_ERROR;
		}
	} else {
		xr->xattr->cullslot = t->index;
		if (_sx(xr->fd, slot_xattr, xr->xattr, xr->len)) {
			debug(0, "Failed to repair slot manually.");
			rc = E_ERROR;
		}
	}

	return rc;
}


/**
 * delete_file does what the name on the tin says.
 * This function is intended to be called on files verified
 * to be orphans -- verified to have NO index entry whatsoever.
 *
 * @param dirfd A file descriptor to the directory we're deleting from.
 * @param de The directory entry structure describing the doomed file.
 * @param bound Boolean: Have we already bound the cache?
 *
 * @return 0 on success, an errno-compatible rc otherwise.
 */
int delete_file(int dirfd, struct dirent *de, int bound)
{
	int rc;
/*
 * delete_file is called:
 * 1) on files with a bad name/type combo (e.g, a dir named D... or a file named +...)
 * 2) When scan_file returns EEXIST
 *    a) When the file has no xattrs
 *    b) When the file has an out of bounds cullslot (but not PINNED)
 *    c) When the file points to a slot with the wrong filehandle**
 *	 **We assume this to mean this file is an orphan, because all index
 *				    entries were repaired in the table scan.
 *
 * In all cases, we find it appropriate to use the CULL command.
 * For orphaned objects, cull will correctly identify via validate_slot that
 *		 the object is bad and will not delete entries* in the index.
 * Out of bounds objects are intercepted by cachefiles_cx_validate_slot and are assumed stale.
 * For objects without xattrs, it will assume it has a stale slot and delete it.
 */
	if (bound) {
		int len;
		char cmdbuff[NAME_MAX + 30];

		len = snprintf(cmdbuff, NAME_MAX + 30, "cull %s", de->d_name);
		if (len >= NAME_MAX + 30) {
			rc = ENAMETOOLONG;
			debug(0, "Error preparing cull command for file [%s]",
			      de->d_name);
			return rc;
		}

		rc = write(cachefd, cmdbuff, len);
		if (rc < len) {
			rc = errno;
			dperror("Failed to send command: %s", cmdbuff);
			return rc;
		} else {
			rc = 0;
		}

	} else {
		/* returns 0 or an errno */
		rc = destroy_file(dirfd, de);
	}

	return rc;
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
 * Retrieve basic cache statistics,
 * including xattrs, filesize, and so on.
 * Perform basic sanity checks on xattrs,
 * filesizes, and so on.
 *
 * @param s The cache to operate on.
 * s->indexfile and s->atimefile must be set.
 * @return 0 on success, an errno-compatible rc otherwise.
 */
int read_state(struct cachefilesd_state *s)
{
	struct stat st;
	int rc = 0;
	char a, b;
	size_t trunc;

	/* Check for presence of .lock file */
	chdir(s->rootdir);
	if (access(".lock", F_OK) == 0 && !s->bound) {
		/*
		 * If we haven't bound to the cache yet,
		 * but there is a .lockfile, it is stale.
		 */
		warning("Stale .lock file detected.");
		s->need_fsck = 1;
	}


	/* Check for presence of index and atimes files.
	 * If they aren't here, that's OK! */
	if ((a = access(s->indexfile, F_OK)) == -1) {
		if (errno != ENOENT) {
			rc = errno;
			dperror("Failed to access [%s]", s->indexfile);
			return rc;
		}
	}
	if ((b = access(s->atimefile, F_OK)) == -1) {
		if (errno != ENOENT) {
			rc = errno;
			dperror("Failed to access [%s]", s->atimefile);
			return rc;
		}
	}

	if ((a == -1) && (b == -1)) {
		/* If both files don't exist, that's OK. */
		return 0;
	} else if (b == -1) {
		/* Only the atimes file exists: let's just delete it. */
		unlink(s->atimefile);
		return 0;
	} else if (a == -1) {
		int fd;
		/* Only the cull_index file exists -- create a new, blank atimes file.
		 * Allow the code below to extend the size of this file as needed. */
		fd = creat(s->atimefile, (S_IRUSR|S_IWUSR));
		if (fd < 0) {
			rc = errno;
			dperror("cull_atimes is missing, but I was unable to create a new one");
			goto leave;
		}
		close(fd);
	}


	/* Retrieve the entity size */
	if ((rc = gx(s->indexfile, index_xattr,
		     "%02x", &s->ent_size))) {
		debug(0, "gx(%s) failed.\n", s->indexfile);
		goto leave;
	}
	if (s->ent_size == 0) {
		errno = rc = EINVAL;
		dperror("The stored xattr size in the culling index CANNOT be zero");
		goto leave;
	}

	/* Retrieve the atime_base.
	 * Make sure to use the right format and vartype,
	 * Or gremlins will eat your stack. */
	if ((rc = gx(s->atimefile, atime_xattr,
		     "%016lx", &s->atime_base))) {
		/* Treat this as a soft error: The kernel will fix this,
		 * and we don't really NEED it to do the rest of our fsck. */
		warning("error retrieving atime_base from s->atimefile.");
		s->need_fsck = 1;
	}

	/* Record the pagesize */
	s->pagesize = getpagesize();


	/* Investigate the index file. */
	do {
		/* Retrieve stats from the index file */
		if (stat(s->indexfile, &st) != 0) {
			rc = errno;
			dperror("Failed to stat index file");
			goto leave;
		}

		/* compute the number of entities per page. */
		s->index_size = st.st_size;
		s->num_perpage = (s->pagesize / s->ent_size);
		s->num_indices = (st.st_size / s->pagesize) * s->num_perpage;

		/* Verify that the index file is sized reasonably. */
		if (!(s->index_size % s->pagesize)) break;

		s->need_fsck = 1;
		debug(1, "Issue: index not a multiple of the pagesize.\n");
		if (truncate(s->indexfile,
			     mult_ceil(s->index_size, s->pagesize)) != 0) {
			dperror("Failed to extend the index to be a multiple "
				"of the pagesize.");
			rc = errno;
		}

	} while (s->index_size % s->pagesize);


	/* Investigate the atimes file. */
	do {
		/* Retrieve stats from the atimes file. */
		if (stat(s->atimefile, &st) != 0) {
			rc = errno;
			dperror("Failed to stat atimes file");
			goto leave;
		}
		s->atime_size = st.st_size;
		s->num_atimes = (st.st_size / sizeof(atime_t));

		/* Ensure the atimes file is sized reasonably. */
		if (!(s->atime_size % sizeof(atime_t)) &&
		    s->num_indices == s->num_atimes) break;

		s->need_fsck = 1;
		debug(1, "Issue: atimes filesize is not a multiple of (%lu * %u).\n",
		      sizeof(atime_t), s->num_perpage);

		/* Number of indices times size of atime,
		 * rounded up to the nearest entities-per-page boundary. */
		trunc = mult_ceil(s->num_indices * sizeof(atime_t),
				  s->num_perpage * sizeof(atime_t));

		if (truncate(s->atimefile, trunc) != 0) {
			dperror("Failed to extend the atimes file to "
				"be a multiple of %lu", sizeof(atime_t) * s->num_perpage);
			rc = errno;
		}

	} while ((s->atime_size % sizeof(atime_t)) ||
		 (s->num_indices != s->num_atimes));


	/* * * * * * * * * * * * * * * * * * * * * * * * * */
	debug(2, "entsize: %d; atime_base: %lu",
	      s->ent_size, s->atime_base);
	debug(2, "index size: %lu; atimes size: %lu",
	      s->index_size, s->atime_size);
	debug(2, "perpage: %d; num: %d; anum: %d",
	      s->num_perpage, s->num_indices,
	      s->num_atimes);
	/* * * * * * * * * * * * * * * * * * * * * * * * * */

	/* the state has now been properly read. */
	s->read = 1;

leave:
	return rc;
}


/**
 * A brief predicate function for determining if
 * the name of a file matches its filetype.
 * @param d_name The name of the file.
 * @param st The file's stat64 structure.
 * @return false if the file does not belong here.
 */
int is_expected(char *d_name, struct stat64 *st)
{
	if (memchr("IDSJET+@", d_name[0], 8) == NULL) {
		return 0;
	}

	if (!S_ISDIR(st->st_mode) &&
	    (!S_ISREG(st->st_mode) ||
	     (memchr("IJ@+", d_name[0], 4) != NULL)))
		return 0;

	return 1;
}


/**
 * name_to_handle_at helper utility.
 * @param dirfd Where to convert to a handle at.
 * @param name The name of the file to get a handle for.
 * @param size How much space to allocate for the handle.
 * @return NULL on failure (see errno),
 * file_handle* on success. You will need to free this memory.
 */
struct file_handle *ntha(int dirfd, const char *name, size_t size)
{
	int mnt_id;
	struct file_handle *fh;

	fh = malloc(sizeof(struct file_handle) + size);
	if (!fh) {
		dperror("Failed to allocate memory for file_handle");
		return NULL;
	}
	fh->handle_bytes = size;

	if (name_to_handle_at(dirfd, name, fh, &mnt_id, 0) != 0) {
		int rc = errno;
		dperror("Failed to convert [%s] to a file_handle", name);
		free(fh);
		errno = rc;
		return NULL;
	}

	return fh;
}


/**
 * scan_file verifies, from the point of view of the file,
 * that everything appears to be OK.
 * We will verify that we have sane xattrs and that our
 * xattr points back to the correct slot in the index,
 * which means that our filehandle matches the one
 * stored in the index.
 *
 * Anything that does not align properly is deleted,
 * under the assumption that anything incorrect is an
 * orphaned file, because any simple inconsistencies
 * were previously fixed by a call to scan_table.
 *
 * @param s The current scan status/state. Needed for reading the index.
 * @param dh A handle to the "current" scan directory.
 * @param de A handle to the file to investigate.
 * @param st a stat64 structure generated on the de previously.
 *
 * @return 0 if the file was OK,
 * EEXIST if the file should be deleted, and -1 otherwise.
 */
int scan_file(struct cachefilesd_state *s,
	      DIR *dh, struct dirent *de,
	      struct stat64 *st)
{
#define _offset(SLOT) (foffset((SLOT), s->pagesize, s->num_perpage, s->ent_size))

	struct cache_xattr *xattr = NULL;
	unsigned len, i;
	int rc = 0;
	struct file_handle *fh, *parentfh = NULL;
	struct index_record *r = malloc(s->ent_size);
	char wrong_fh = 0; /* indicator flag */

	/* Intermediate nodes have no data, that's OK. See you later! */
	if ((de->d_name[0] == '@') || (de->d_name[0] == '+')) {
		debug(3, "Skipping [%s]: Intermediate index.", de->d_name);
		goto free_ir;
	}

	rc = bx(de->d_name, slot_xattr, (char **)&xattr, &len);

	if (rc == ENOATTR) {
		debug(1, "[%s] doesn't have the correct xattrs.", de->d_name);
		/* Note that we signal a file to be deleted by returning EEXIST. */
		rc = EEXIST;
		goto free_ir;
	} else if (rc) {
		debug(0, "Error retrieving xattrs from file.");
		goto free_ir;
	}

	debug(2, "slot: %03u; file: [%s]; ", xattr->cullslot, de->d_name);

	if (xattr->cullslot == CACHEFILES_PINNED) {
		debug(3, "[%s] is pinned\n", de->d_name);
		goto free_xattr;
	}

	/* Out of bounds cullslot, includes NO_CULL_SLOT. */
	if (xattr->cullslot > s->num_indices) {
		debug(1, "File has an out-of-bounds cullslot (%u > %u)\n",
		      xattr->cullslot, s->num_indices);
		rc = EEXIST;
		goto free_xattr;
	}

	/* Now, we're going to do a lot of mucking around in order to test
	 * reciprocation, which includes slot number and file handle agreement.
	 * We're going to get our file_handle and perhaps
	 * our parent's file handle. */

	/* Obtain a file_handle for this dirent. */
	fh = ntha(dirfd(dh), de->d_name, MAX_HANDLE_SZ);
	if (!fh) {
		rc = errno;
		goto free_xattr;
	}

	/* And we get one for the parent to make sure the dirent
	 * is in the right directory. */
	if (!S_ISDIR(st->st_mode)) {
		parentfh = ntha(dirfd(dh), ".", MAX_HANDLE_SZ);
		if (!parentfh) {
			rc = errno;
			goto free_fh;
		}
	}

	debug_nocr(2, "\thandle: ");
	for (i = 0; i < fh->handle_bytes; ++i) {
		debug_nocr(2, "%02x", fh->f_handle[i]);
	}
	if (parentfh) {
		for (i = 0; i < parentfh->handle_bytes; ++i) {
			debug_nocr(2, "%02x", parentfh->f_handle[i]);
		}
	}
	debug_nocr(2, "\n");

	/* Check on our memory for the entity/record */
	if (!r) {
		dperror("Failed to allocate memory for index record");
		rc = errno;
		goto free_pfh;
	}

	/* open the index if it hasn't been already */
	if (!s->indexfh) {
		s->indexfh = fopen(s->indexfile, "r");
		if (!s->indexfh) {
			rc = errno;
			dperror("Failed to open index for reading");
			goto free_pfh;
		}
	}


	/* Read in the entry of interest */
	if (fseek(s->indexfh, _offset(xattr->cullslot), SEEK_SET)) {
		dperror("failed to seek filestream");
		rc = errno;
		goto free_pfh;
	}
	if (fread(r, s->ent_size, 1, s->indexfh) != 1) {
		dperror("failed to read index");
		rc = errno;
		goto free_pfh;
	}

	/* If we are a directory, compare only our file handle.
	 * If we are a regular file, check ours and our parent's.
	 */
	if (memcmp(r->fh, fh->f_handle, fh->handle_bytes) != 0) {
		wrong_fh = 1;
	}
	if (parentfh) {
		if (memcmp(&r->fh[fh->handle_bytes], parentfh->f_handle,
			   parentfh->handle_bytes) != 0) {
			wrong_fh = 1;
		}
	}

	if (wrong_fh) {
		debug(1, "Error: file_handles differ. Removing object.\n");
		rc = EEXIST;
		goto free_pfh;
	}

free_pfh:
	if (parentfh) free(parentfh);
free_fh:
	free(fh);
free_xattr:
	free(xattr);
free_ir:
	free(r);
	if (!rc) return 0;
	return rc == EEXIST ? rc : -1;
}


/**
 * Given a DIR handle, iterate through the objects in that directory,
 * validating files and recursing on further directories found.
 * @param s cachefilesd_state object describing the folders to spider through
 * @param relpath A directory relative to the CWD to enter and scan.
 * @return 0 if everything went OK.
 */
int spider(struct cachefilesd_state *s, const char *relpath, int *num)
{
	DIR *dh;
	struct dirent dirent, *de;
	struct stat64 st;
	int rc = 0;
	*num = 0;


	/* Open up this directory. */
	dh = opendir(relpath);
	if (dh == NULL) {
		rc = errno;
		if (rc == ENOENT) {
			/* Soft error: File is gone. */
			debug(1, "Rocky footing: [%s] went missing",
			      relpath);
			return 0;
		}
		dperror("Couldn't open directory");
		return rc;
	}

	/* Change CWD to this directory. */
	if (fchdir(dirfd(dh)) < 0)
		oserror("Failed to change current directory");

	/* In general, keep reading files until we can't. */
	while (1) {

		/* Iterate through this directory. */
		if (readdir_r(dh, &dirent, &de) < 0) {
			if (errno == ENOENT) {
				debug(1, "Warning: File was already deleted.");
				break;
			}
			dperror("Error: Problem reading directory");
			rc = -1;
			break;
		}

		/* Are we done in this dir? */
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
		if (fstatat64(dirfd(dh), dirent.d_name, &st, 0) < 0) {
			if (errno == ENOENT) {
				debug(1, "Warning: Rocky Footing.");
				(*num)--;
				continue;
			}
			debug(0, "Error: Failed to stat directory.");
			rc = -1;
			break;
		}

		/* Make sure filename looks correct,
		 * and ensure the prefix letter matches the actual filetype. */
		if (!is_expected(dirent.d_name, &st)) {
			debug(1, "[%s] has a bad name,"
			      " or bad name/type combo. Deleting.", de->d_name);
			(*num)--;
			rc = delete_file(dirfd(dh), de, s->bound);
			if (rc) {
				debug(0, "Error removing clutter file [%s]", de->d_name);
				(*num)++;
			}

			continue;
		}

		/* Descend first, if applicable. */
		if (S_ISDIR(st.st_mode)) {
			int child_num;
			rc = spider(s, de->d_name, &child_num);
			if (rc) break;

			/* Assume that, if spider() reports 0 entries,
			 * that it deleted the empty subdirectory. */
			if (DELETE_EMPTY_DIRS && !child_num) {
				(*num)--;
			}
		}

		/* If we've made it this far, we've got a candidate file to
		 * scan for validity. Pass our cache-wide struct s, the DIR
		 * handle, and the dirent, and the states over. */
		rc = scan_file(s, dh, de, &st);
		if (rc == EEXIST) {
			(*num)--;
			rc = delete_file(dirfd(dh), de, s->bound);
			if (rc) {
				(*num)++;
				debug(0, "Failed to delete %s.", de->d_name);
				break;
			}
		} else if (rc) {
			dperror("scan_file failed on %s", de->d_name);
			break;
		}
	}

	/* At this point, we're done iterating through this folder.
	 * Move back up one directory, so that our parent is
	 * where it left off. */
	closedir(dh);

	if (chdir("..")) {
		rc = errno;
		dperror("Couldn't go back up!\n");
		return rc;
	}

	/* If we found the directory was empty, kill it. */
	if (DELETE_EMPTY_DIRS && !*num) {
		debug(1, "Removing empty directory (%s)", relpath);
		rc = rmdir(relpath);
		if (rc) {
			dperror("Unable to unlink directory presumed to be empty. (%s)",
				relpath);
		}
	}

	return rc;
}


/**
 * scan_slot will, given an index, verify that:
 * (1) the indexed entry points to a valid file
 * (2) That file has an xattr,
 * (3) That xattr contains the correct index.
 *
 * In the negative case of (1) and (2), we blank the index entry.
 * In the case of (3), we repair the xattr.
 *
 * @param s Contains cache information, buffers and statistics.
 * @param t Contains entry information, including page and index.
 *
 * @return either E_OK, E_ERROR or E_DIRTY, indicating that
 * we either did nothing, changed memory, or encountered a whoopsie.
 */
scan_rc scan_slot(struct cachefilesd_state *s,
		  struct scan_state *t)
{
#define err(cond,str,label) errchk_e(cond, rc, E_ERROR, str, label)

	int dirfd, fd;
	unsigned xattr_len, i;
	int rc = E_OK;
	struct cache_xattr *xattr = NULL;
	struct index_record *r = t->r;
	struct file_handle *fh = malloc(sizeof(struct file_handle) + r->len);

	err(!fh, "failed to malloc", leave);

	/* Debug print-info of this record. */
	if (!t->duplicate_pass) {
		unsigned short k;
		const slot_t index = t->index;
		const struct index_record *r = t->r;

		debug(2, "index: %08u; atime: %08u; type: %02x;"
		      "len: %02x; handle: 0x",
		      index, t->abuffer[t->local_index], r->type, r->len);

		/* Loop through the bytes in the record. */
		for (k = 0; k < r->len; ++k) {
			debug_nocr(2, "%02x", r->fh[k]);
		}
		debug_nocr(2, "\n");
	}

	/* Copy our record into a format that syscalls will understand */
	fh->handle_bytes = r->len;
	fh->handle_type = r->type;
	memcpy(fh->f_handle, r->fh, r->len);

	/* Open the root cache dir */
	dirfd = open(s->rootdir, O_DIRECTORY);
	err(dirfd < 0, "open (dir) failed.\n", free1);

	/* Open the file referenced. */
	fd = open_by_handle_at(dirfd, fh, 0);
	if (fd < 0) {
		if (errno == ESTALE) {
			debug(1, "Stale file handle in index: Deleting slot.");
			rc = delete_slot(r, t, s->bound);
		} else {
			dperror("open_by_handle_at failed");
			rc = E_ERROR;
		}
		goto close1;
	};

	/* Obtain the xattrs of the open file */
	rc = _bx(fd, slot_xattr, (char **)&xattr, &xattr_len);
	if (rc == ENOATTR) {
		debug(1, "Suspected stale filehandle: slot #%u points to a "
		      "file with missing xattr property. Deleting slot.", t->index);
		rc = delete_slot(r, t, s->bound);
		goto close2;
	} else if (rc) {
		debug(0, "Error obtaining xattrs for slot object #%u", t->index);
		goto close2;
	}

	/* Debug: Print the entire xattr for viewing. */
	for (i = 0; i < xattr_len - sizeof(uint32_t); ++i) {
		debug_nocr(4, "%02x", xattr->data[i]);
	}
	debug_nocr(4, "\n");

	/* Gaze into the void as the void gazes into us.
	 * Ensure that the slot we point to points back. */
	if (t->index != xattr->cullslot) {

		/* If this file points to the wrong slot, but we're on our
		 * second pass, this means that this entry is a duplicate
		 * and can be removed. If the file is marked as PINNED,
		 * it should likely not be in the culling index. */
		if (xattr->cullslot == CACHEFILES_PINNED) {
			debug(1, "Slot #%u points to a PINNED file. Removing this slot.",
			      t->index);
			rc = delete_slot(r, t, s->bound);
		}

		else if (t->duplicate_pass) {
			debug(1, "Slot #%u points to the wrong slot (#%u), this slot is likely a duplicate.",
			      t->index, xattr->cullslot);
			rc = delete_slot(r, t, s->bound);
		}

		/* Otherwise, try to repair it. */
		else {
			struct xattr_repair xr = {
				.xattr = xattr,
				.len = xattr_len,
				.fd = fd
			};

			debug(1, "Slot #%u points to a file which "
			      "points back to slot #%u. Correcting xattrs.\n",
			      t->index, xattr->cullslot);

			rc = repair_slot(t, &xr, s->bound);
			if (rc != E_OK) {
				debug(0, "repair_slot did not return E_OK");
				goto free2;
			}
		}

		goto free2;
	}

	/* Tidy up. */
free2:
	free(xattr);
close2:
	close(fd);
close1:
	close(dirfd);
free1:
	free(fh);
leave:
	return rc;
}


/**
 * scan_page confirms the validity of each index entry,
 * one page's worth of index entries at a time.
 * @param s The cachefilesd state structure.
 * @param t The scan status structure telling us which page to scan.
 * @return scan_rc -- E_OK if everything is ok,
 * E_DIRTY if there were changes that were made to the index,
 * E_ERROR if we failed to complete the scan.
 */
scan_rc scan_page(struct cachefilesd_state *s,
		  struct scan_state *t)
{
#define record_at(D,J) (D[(J)*(s->ent_size)])

	unsigned j;
	scan_rc tmp_rc, s_rc = E_OK;
	t->index = t->pageno * s->num_perpage;

	/* loop through the records in the page. */
	for (j = 0; j < s->num_perpage; ++j, ++t->index) {
		t->local_index = j;
		t->r = (struct index_record *) &(record_at(t->buffer,j));

		/* Nothing to do if it is empty. */
		if (is_empty(t->r)) {
			if (t->abuffer[j]) {
				debug(0, "Index inconsistency: slot (%u) is unused but atime is non-zero. (%u)",
				      t->index, t->abuffer[j]);
				tmp_rc = delete_slot(t->r, t, s->bound);
			} else {
				tmp_rc = E_OK;
			}
		} else {
			/* Scan this individual record. */
			tmp_rc = scan_slot(s, t);
		}


		if (tmp_rc == E_OK) continue;
		else if (tmp_rc == E_DIRTY) s_rc = E_DIRTY;
		else return E_ERROR;
	}

	/* The page read is over, now to act on it: */
	if (s_rc == E_DIRTY) {
		debug(1, "Page is dirty, recommitting to disk; page:%lu offset:%lu",
		      t->pageno, s->pagesize * t->pageno);

		tmp_rc = fseek(s->indexfh, s->pagesize * t->pageno, SEEK_SET);
		if (tmp_rc) {
			dperror("Failed to seek to position in culling index");
			return E_ERROR;
		}

		tmp_rc = fseek(s->atimefh, t->pageno * s->num_perpage * sizeof(atime_t),
			       SEEK_SET);
		if (tmp_rc) {
			dperror("Failed to seek to position in atime index");
			return E_ERROR;
		}

		tmp_rc = fwrite(t->buffer, s->ent_size, s->num_perpage, s->indexfh);
		if (tmp_rc != s->num_perpage) {
			dperror("Failed to recommit dirty index page back to disk");
			return E_ERROR;
		}

		tmp_rc = fwrite(t->abuffer, sizeof(atime_t), s->num_perpage, s->atimefh);
		if (tmp_rc != s->num_perpage) {
			dperror("Failed to recommit dirty atime page back to disk");
			debug(0,"WARNING: Recommitted index, but not atimes.\n"
			      "Indices may now be out of sync. Re-run an index check.");
			return E_ERROR;
		}
	}

	return s_rc;
}


/**
 * scan_table verifies all of the indices in the culling index.
 * @param s The state structure describing the index to scan.
 * @return 0 if everything went OK.
 */
int scan_table(struct cachefilesd_state *s)
{

	int rc = 0;
	unsigned rb;
	unsigned npages = (s->index_size / s->pagesize);
	struct scan_state *t;

	err_chk_rc(!(t = calloc(sizeof(struct scan_state),1)),
		   "Failed to allocate scan state structure", leave);

	/* A page of entities' worth of atimes. */
	err_chk_rc(!(t->abuffer = calloc(sizeof(atime_t),s->num_perpage)),
		   "Failed to allocate space for atimes array", unmalloc1);

	/* A pages worth of records. */
	err_chk_rc(!(t->buffer = calloc(s->ent_size,s->num_perpage)),
		   "Failed to allocate space for page", unmalloc2);

	if (!s->indexfh)
		err_chk_rc(!(s->indexfh = fopen(s->indexfile, "r+")),
			   "Failed to open index file", unmalloc3);

	if (!s->atimefh)
		err_chk_rc(!(s->atimefh = fopen(s->atimefile, "r+")),
			   "Failed to open atimes file", close1);

	t->duplicate_pass = 0;

	/* Read both indices, page-by-page. */
pageloop:
	for (t->pageno = 0; t->pageno < npages; ++t->pageno) {

		fseek(s->indexfh, (t->pageno * s->pagesize), SEEK_SET);
		fseek(s->atimefh,
		      (t->pageno * s->num_perpage * sizeof(atime_t)),
		      SEEK_SET);

		rb = fread(t->abuffer, sizeof(atime_t),
			   s->num_perpage, s->atimefh);
		err_chk_rc(rb < s->num_perpage,
			   "Failed to read a page's worth of atimes.", close2);

		rb = fread(t->buffer, s->ent_size,
			   s->num_perpage, s->indexfh);
		err_chk_rc(rb < s->num_perpage,
			   "Failed to read a page of record entries.", close2);

		debug(2, "--- (%u) Read page #%lu. ---", getpid(), t->pageno);

		/* Do our error-checking on this chunk. */
		rc = scan_page(s, t);
		if (rc == E_DIRTY || rc == E_OK) rc = 0;
		else {
			debug(0, "Error analyzing/repairing page #%lu.", t->pageno);
			rc = E_ERROR;
			goto close2;
		}
	}

	/* Do a duplicate-detection run. */
	if (!t->duplicate_pass) {
		sync();
		t->duplicate_pass = 1;
		goto pageloop;
	}

close2:
close1:
unmalloc3:
	free(t->buffer);
	t->buffer = NULL;
unmalloc2:
	free(t->abuffer);
	t->abuffer = NULL;
unmalloc1:
	free(t);
leave:
	return rc;
}


/**
 * cachefilesd_fsck_deep is a function that encapsulates both
 * scan_table (fixing the index) and spider (fixing the files.)
 * @param s The cachefilesd state structure.
 * @return 0 if everything went OK.
 */
int cachefilesd_fsck_deep(struct cachefilesd_state *s)
{
	int rc;
	int num;

	/* Scan and fix the index, if there is one.
	 * It is not an error if the index is absent;
	 * the kernel may have yet to create it for us. */
	if (s->read) {
		rc = scan_table(s);
		if (rc) {
			debug(0, "Failed to scan the culling index.");
			return rc;
		}
	}

	sync();

	/* Jump to the top of the cache */
	rc = chdir(s->rootdir);
	if (rc) {
		dperror("Failed to change directory to [%s]", s->rootdir);
		return rc;
	}

	/* Check for orphaned and junk files in the cache.
	 * If the directory does not exist, that's ok -- it might not exist yet.
	 */
	if (access("cache", F_OK) == 0) {
		if ((rc = spider(s, "cache", &num))) {
			debug(0, "spidering through the cachedir failed\n");
			return rc;
		}
	}

	/*
	 * If we didn't engage the kernel and found no errors,
	 * delete the .lock file. Otherwise, we assume the kernel
	 * is active and expects the lock file to be present.
	 */
	if (!rc && !s->bound) {
		if (chdir(s->rootdir)) {
			rc = errno;
			dperror("Failed to change directories to rootdir=(%s)",
				s->rootdir);
			return rc;
		}
		if (unlink(".lock") && errno != ENOENT) {
			rc = errno;
			dperror("Failed to unlink .lock file.");
			return rc;
		}
	}

	/* If we are running 'online,' bound to the kernel module,
	 * Tell the kernel the results of the scan. */
	if (s->bound) {
		unsigned char len, code = rc;
		char sbuff[10];

		len = snprintf(sbuff, 10, "fsck %d", code);
		debug(0, "sending cmd: [%s]", sbuff);
		if (write(cachefd, sbuff, len) < len) {
			dperror("Error reporting status to kernel");
		}
	}

	if (s->indexfh) {
		fclose(s->indexfh);
		s->indexfh = NULL;
	}

	if (s->atimefh) {
		fclose(s->atimefh);
		s->atimefh = NULL;
	}

	return 0;
}


/**
 * cachefilesd_fork will fork off a process to perform a deep scan 
 * of the culling index, and manage the needed state to do so.
 * @param s The cachefiles cache to fork off with and scan.
 * @return An errno compatible return code. (0 on success.)
 */
int cachefilesd_fork(struct cachefilesd_state *s)
{
	pid_t pid;
	int rc;

	/* bookmark this state struct for the sighandler */
	fork_state = s;

	s->fsck_running = 1;
	debug(2, "in fork() -- parent pid = %u", getpid());

	switch ((pid = fork())) {
	case -1:
		rc = errno;
		dperror("Failed to fork the fsck process.");
		s->fsck_running = 0;
		return rc;
	case 0:
		/* Do not let the child out of the box */
		debug(2, "in fork() -- child pid = %u", getpid());
		if (setpriority(PRIO_PROCESS, syscall(SYS_gettid), 19)) {
			rc = errno;
			dperror("Failed to adjust scanning process niceness");
			exit(rc);
		}

		rc = cachefilesd_fsck_deep(s);
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
 * @param sig incoming signal
 */
void cachefilesd_sigchld(int sig)
{
	pid_t pid;
	int rc = 0;
	pid = wait(&rc);
	debug(1, "fsck process (%u) exited. got rc = %d", pid, rc);

	if (!fork_state) {
		debug(0, "Error: a child process has exited, but I can't map its PID to a cache.");
		stop = 1;
	}

	if (!rc) {
		debug(1, "fsck completed successfully.");
	} else {
		debug(0, "Error: fsck encountered problems. Halting daemon.");
		stop = 1;
	}

	/* Mark as zero regardless so we don't spawn an extra scan
	 * if we are exiting due to error. */
	fork_state->need_fsck = 0;
	fork_state->fsck_running = 0;
}

/**
 * state_init initializes the cachefilesd_state structure
 * by performing the menial tasks of allocating strings
 * and so forth that point to paths of interest.
 * @param state A handle to the state pointer, so we can allocate the structure.
 * @param root A const string indicating the fscache root directory.
 * @see state_destroy for cleanup.
 * @return -1 on internal failure, an errno-compatible return code otherwise.
 */
int state_init(struct cachefilesd_state **state, const char *root)
{
	int rc;
	struct cachefilesd_state *s;

	if (!state) {
		debug(0, "state_init requires a valid (state **) pointer.");
		return -1;
	}

	s = malloc(sizeof(struct cachefilesd_state));
	if (!s) {
		rc = errno;
		dperror("Could not allocate space for cachefilesd_state structure");
		return rc;
	}
	*state = s;

	s->rootdir = root;
	s->atimefh = NULL;
	s->indexfh = NULL;

	s->indexfile = str2cat(root, "/cull_index");
	if (!s->indexfile) return -1;

	s->atimefile = str2cat(root, "/cull_atimes");
	if (!s->atimefile) {
		free(s->indexfile);
		s->indexfile = NULL;
		return -1;
	}

	s->pagesize = getpagesize();
	s->read = 0;
	s->bound = 0;
	s->need_fsck = 0;
	s->fsck_running = 0;

	s->init = 1;
	return 0;
}


/**
 * state_destroy frees any memory and closes any open handles
 * that were opened by state_init.
 * @param state A pointer to the handle of the cachefilesd_state structure to destroy.
 * @return void.
 */
void state_destroy(struct cachefilesd_state **state)
{
#define ifnnull(func,ptr) do {			\
		if (ptr) {			\
			func(ptr);		\
			ptr = NULL;		\
		}				\
	} while (0)

	struct cachefilesd_state *s = *state;
	if (s == NULL) return;

	ifnnull(free, s->indexfile);
	ifnnull(free, s->atimefile);
	ifnnull(fclose, s->atimefh);
	ifnnull(fclose, s->indexfh);
	ifnnull(free, s);
	*state = NULL;
}


/**
 * cachefilesd_fsck_light initializes the cachefilesd_state structure,
 * performs a preliminary scan, and makes a note if a closer inspection
 * is warranted. How (and if) the deep scan is launched is up to the
 * caller, by investigating (*state)->need_fsck.
 * @param cacheroot The root directory of the cache to scan.
 * @param state A handle to some threading info in case we background a thread.
 * @return 0 if everything went OK.
 */
int cachefilesd_fsck_light(const char *cacheroot, struct cachefilesd_state **state)
{
	int rc = 0;

	if (*state == NULL) {
		rc = state_init(state, cacheroot);
		if (rc) {
			debug(0, "Failed to initialize scanning state structure.");
			return rc;
		}
	}

	info("Scan started.\n"
	     "Cacheroot: [%s]\n"
	     "Index: [%s]\n"
	     "Atimes: [%s]\n",
	     cacheroot,
	     (*state)->indexfile,
	     (*state)->atimefile);

	/* Retrieve filesizes and cache-wide xattrs. */
	if ((rc = read_state(*state))) {
		debug(0, "Error: Failed to retrieve cachefiles state. "
		      "Cache index may be corrupt. Unable to verify.");
		return rc;
	}

	/* If the user has requested a deep scan, or we found preliminary issues --
	 * such as a stale lockfile or bad filesizes -- do a deep scan. */
	if (!(*state)->need_fsck) {
		info("Cache appears clean. If you wish to FORCE a deep scan, re-run with -F.");
	}

	/* Register a sighandler for scanning, to prepare for the
	 * future deep scan effort. */
	signal(SIGCHLD, cachefilesd_sigchld);

	return rc;
}
