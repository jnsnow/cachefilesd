#ifndef __XATTR_H
#define __XATTR_H

/* attr/xattr.h is a naughty header and
 * requires stdlib and stdio to be included
 * BEFORE it. */
#include <stdio.h>
#include <stdlib.h>
#include <attr/xattr.h>
#include <stdint.h>

/**
 * For generic queries of unknown types.
 */
struct generic_xattr {
	int fd;		    /**< Optional; save the FD associated with this xattr. */
	unsigned long len;  /**< The length of the xattr data after retrieval. */
	uint8_t xdata[];    /**< zero-width binary handle to xattr data. */
} __attribute__ ((packed));

/* List xattrs */
int _lx(int fd, FILE *stream);
int lx(const char *path, FILE *stream);

/* Buffer xattr (read and copy to buffer.) */
int _bx(int fd, const char *xattr, struct generic_xattr **ptr);
int bx(const char *file, const char *xattr, struct generic_xattr **ptr);

/* Get xattrs (buffer and scanf into a value) */
int _gx(int fd, const char *xattr, const char *fmt, void *value);
int gx(const char *file, const char *xattr, const char *fmt, void *value);

/* Replace an existing xattr with a new one. */
int _sx(int fd, const char *xattr, const void *ptr, unsigned len);

#endif
