#ifndef __XATTR_H
#define __XATTR_H

/* attr/xattr.h is a naughty header and
 * requires stdlib and stdio to be included
 * BEFORE it. */
#include <stdio.h>
#include <stdlib.h>
#include <attr/xattr.h>

/* List xattrs */
int _lx(int fd, FILE *stream);
int lx(const char *path, FILE *stream);

/* Buffer xattr (read and copy to buffer.) */
int _bx(int fd, const char *xattr, char **ptr, unsigned *len);
int bx(const char *file, const char *xattr, char **ptr, unsigned *len);

/* Get xattrs (buffer and scanf into a value) */
int _gx(int fd, const char *xattr, const char *fmt, void *value);
int gx(const char *file, const char *xattr, const char *fmt, void *value);

/* Replace an existing xattr with a new one. */
int _sx(int fd, const char *xattr, const void *ptr, unsigned len);

#endif
