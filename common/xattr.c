#include "debug.h"
#include "xattr.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <attr/xattr.h>
#include <string.h>

#include <unistd.h>
#include <fcntl.h>

/**
 * _lx prints to stream all of the xattrs contained in the file fd.
 * @param fd open file we'd like to see the xattrs of
 * @param stream The FILE stream to print the xattrs to.
 * @return An errno-compatible integer.
 */
int _lx(int fd, FILE *stream)
{
	int rc;
	int buffsiz = flistxattr(fd, NULL, 0);
	int size, pos;
	char *buffer;

	if (buffsiz < 0) {
		rc = errno;
		dperror("flistxattr");
		return rc;
	}

	buffer = malloc(buffsiz);
	if (!buffer) {
		rc = errno;
		dperror("no space for xattr string");
		return rc;
	}

	if ((size = flistxattr(fd, buffer, buffsiz)) == -1) {
		rc = errno;
		dperror("flistxattr failed");
		free(buffer);
		return rc;
	}

	for (pos = 0; pos < size - 1;) {
		info("lx: %s", buffer + pos);
		pos += strlen(buffer + pos) + 1;
	}

	free(buffer);
	return 0;
}


/**
 * @see _lx. Uses a path instead of a file descriptor.
 * @param path The file to print attributes for.
 * @param stream The stream to print the attributes to.
 * @return An errno-compatible integer.
 */
int lx(const char *path, FILE *stream)
{
	int rc;
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		rc = errno;
		dperror("Could not open file [%s]", path);
		return rc;
	}

	rc = _lx(fd, stream);
	if (rc) {
		if (close(fd)) {
			int rc2 = errno;
			dperror("Could not close file descriptor");
			return rc2;
		}
		return rc;
	}

	rc = close(fd);
	if (rc) {
		dperror("Could not close file descriptor");
	}

	return rc;
}


/**
 * _bx retrieves the xattribute named by 'xattr' and stores it in a buffer.
 * @param fd The file descriptor to get the xattr for
 * @param name The name of the xattr to retrieve.
 * @param buf A handle to an xattr structure ptr we will use.
 * This function will allocate memory and save the pointer in *buf.
 * We expect *buf to be NULL.
 * @return An errno-compatible integer.
 */
int _bx(int fd, const char *name, struct generic_xattr **buf)
{
	int query;
	struct generic_xattr *buffer;
	int rc = 0;

	if (*buf != NULL) {
		warning("buffer_xattr expects a NULL buffer, "
			"and will overwrite it!");
	}

	query = fgetxattr(fd, name, NULL, 0);
	ret_syserr((query < 0 && errno != ENOATTR),
		   "fgetxattr size estimation failed");
	if (query < 0 && errno == ENOATTR) return ENOATTR;
	buffer = malloc(sizeof(struct generic_xattr) + query + 1);
	ret_syserr((buffer == NULL), "Couldn't allocated memory for xattr query");
	debug(3, "_bx(): query size is %d bytes.\n", query);

	buffer->len = fgetxattr(fd, name, buffer->xdata, query);
	jmp_syserr((buffer->len != query), free1,
		   "fgetxattr failed");

	/* For repair purposes later on, record the fd used. */
	buffer->fd = fd;

	/* cap with NB, so we can treat it like a string. */
	buffer->xdata[query] = 0x0;
	*buf = buffer;
	return 0;
free1:
	free(buffer);
	return rc;
}


/**
 * Buffer Xattr, wrapper to _bx. uses a filename string instead of an fd.
 * This function will allocate memory and save the pointer in *ptr.
 * We expect *ptr to be NULL.
 *
 * @see _bx
 * @param file The filename to open.
 * @param xattr The name of the xattr to retrieve.
 * @param ptr A handle to the xattr buffer to use & initialize.
 * @return An errno-compatible integer.
 */
int bx(const char *file, const char *xattr, struct generic_xattr **ptr)
{
	int rc, fd;

	fd = open(file, O_RDONLY);
	ret_syserr((fd < 0), "Failed to open [%s] to retrieve xattrs",
		   file);

	rc = _bx(fd, xattr, ptr);
	if (rc)
		goto error;

	rc = close(fd);
	ret_syserr((rc != 0), "Could not close file descriptor");
	return 0;
error:
	if (close(fd))
		oserror("Error closing out file descriptor in bx() error handler");
	return rc;
}


/**
 * _gx uses scanf() to retrieve the value of a specific xattr from a file.
 * Note that this implies that the xattr is treated like a string.
 * @param fd The open file to get the xattr from.
 * @param xattr The name of the xattribute to retrieve.
 * @param fmt The scanf format code to treat the data as.
 * @param value The address to store the scanf code into.
 * @return 0 if everything goes OK.
 */
int _gx(int fd, const char *xattr, const char *fmt, void *value)
{
	struct generic_xattr *buffer = NULL;
	int rc = 0;

	/* malloc and obtain the raw data. */
	rc = _bx(fd, xattr, &buffer);
	if (rc) {
		debug(0, "_bx failed under _gx.");
		return rc;
	}

	debug(2, "gx(%s) : {%s}", fmt, buffer->xdata);

	if (sscanf((char *)buffer->xdata, fmt, value) != 1) {
		rc = errno;
		dperror("Bad sscanf conversion");
		goto cleanup;
	}

cleanup:
	free(buffer);
	return rc;
}


/**
 * gx is a wrapper for _gx, but takes a filename instead of an fd.
 * @see _gx
 * @param file Path to the file to get the xattrs of.
 * @param xattr The name of the xattribute to retrieve.
 * @param fmt The scanf format code to treat the data as.
 * @param value The address to store the scanf code into.
 * @return 0 if everything goes OK.
 */
int gx(const char *file, const char *xattr,
       const char *fmt, void *value)
{
	int fd = open(file, O_RDONLY);
	int rc;
	if (fd < 0) {
		dperror("Failed to open file to retrieve xattrs");
		return errno;
	}

	rc = _gx(fd, xattr, fmt, value);
	close(fd);
	return rc;
}


/**
 * _sx Replaces the existing attribute with a new one of an identical length.
 * @param fd The file to set the xattr on.
 * @param xattr The xattr to replace.
 * @param ptr The data to store as the replacement value.
 * @param len The length of the data buffer.
 * @return An errno-compatible integer.
 */
int _sx(int fd, const char *xattr, const void *ptr, unsigned len)
{
	int rc;

	rc = fsetxattr(fd, xattr, ptr, len, XATTR_REPLACE);
	if (rc < 0) {
		rc = errno;
		dperror("Failed to set xattr");
	}

	return rc;
}
