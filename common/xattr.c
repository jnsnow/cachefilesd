#include "debug.h"

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
 * @return 0 on success, or an errno compatible return code.
 */
int _lx(int fd, FILE *stream)
{
	int buffsiz = flistxattr(fd, NULL, 0);
	int size, pos;
	char *buffer;

	if (buffsiz < 0) {
		dperror("flistxattr");
		return errno;
	}

	buffer = malloc(buffsiz);
	if (!buffer) {
		dperror("no space for xattr string");
		return errno;
	}

	if ((size = flistxattr(fd, buffer, buffsiz)) == -1) {
		int rc = errno;
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
 * @return 0 on success, an errno compatible return code otherwise.
 */
int lx(const char *path, FILE *stream)
{
	int rc;
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		dperror("Could not open file");
		return errno;
	}

	rc = _lx(fd, stream);
	close(fd);

	return rc;
}


/**
 * _bx retrieves the xattribute named by 'xattr' and stores it in a buffer.
 * @param fd The file descriptor to get the xattr for
 * @param xattr The name of the xattr to retrieve.
 * @param ptr A handle to a char pointer we will use.
 * @param len A return value for the size of the xattr.
 * This function will allocate memory and save the pointer in *ptr.
 * We expect *ptr to be NULL.
 * @return 0 on success.
 */
int _bx(int fd, const char *xattr, char **ptr, unsigned *len)
{
	int query;
	char *buffer;
	int rc = 0;

	if (*ptr != NULL) {
		warning("buffer_xattr expects a NULL buffer, "
			"and will overwrite it!");
	}

	query = fgetxattr(fd, xattr, NULL, 0);
	if (query < 0) {
		if (errno != ENOATTR)
			dperror("fgetxattr size estimation failed");
		return errno;
	}

	buffer = malloc(sizeof(char) * (query + 1));
	if (!buffer) {
		dperror("Couldn't allocate memory for xattr query");
		return errno;
	}

	debug(3, "_bx(): query size is %d bytes.\n", query);

	/* though technically fgetxattr returns an unsigned,
	 * it is documented to return "-1" on error. */
	*len = fgetxattr(fd, xattr, buffer, query);
	if (*len == -1) {
		rc = errno;
		dperror("fgetxattr failed");
		free(buffer);
		return rc;
	}

	/* cap with NB, so we can treat it like a string. */
	buffer[*len] = 0x0;
	*ptr = buffer;
	return 0;
}


/**
 * wrapper to _bx. uses a filename string instead of an fd.
 * This function will allocate memory and save the pointer in *ptr.
 * We expect *ptr to be NULL.
 *
 * @see _bx
 * @param file The filename to open.
 * @param xattr The name of the xattr to retrieve.
 * @param ptr A handle to a char pointer we will use.
 * @param len A return value for the size of the xattr.
 * @return 0 on success, an errno compatible return code otherwise. 
 */
int bx(const char *file, const char *xattr, char **ptr, unsigned *len)
{
	int fd = open(file, O_RDONLY);
	int rc;
	if (fd < 0) {
		dperror("Failed to open file to retrieve xattrs");
		return errno;
	}

	rc = _bx(fd, xattr, ptr, len);
	close(fd);
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
	char *buffer = NULL;
	int rc = 0;
	unsigned len;

	/* malloc and obtain the raw data. */
	rc = _bx(fd, xattr, &buffer, &len);
	if (rc) {
		debug(0, "_bx failed.");
		return rc;
	}

	debug(2, "gx(%s) : {%s}", fmt, buffer);

	if (sscanf(buffer, fmt, value) != 1) {
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
 * @return 0 if everything is OK.
 */
int _sx(int fd, const char *xattr, const void *ptr, unsigned len)
{
	int rc;

	rc = fsetxattr(fd, xattr, ptr, len, XATTR_REPLACE);
	if (rc < 0) {
		dperror("Failed to set xattr");
	}

	return rc;
}
