#ifndef __CACHEFILESD_H
#define __CACHEFILESD_H

#include <stdlib.h>
#include <limits.h>
#include <stdint.h>
#include <dirent.h>

/* Some special values for cullslots. */
enum cullslot {
	CACHEFILES_NO_CULL_SLOT = UINT_MAX,
	CACHEFILES_PINNED	= (UINT_MAX - 1)
};

/* In case the xattr representation of time is changed
 * to accomodate caches in use for longer than 136 years */
typedef uint32_t atime_t;
typedef uint32_t slot_t;

extern int destroy_file(int dirfd, struct dirent *de);

extern void cachefilesd_cleanup(void);

/* Exported so that the fsck thread can request the daemon to stop. */
extern int stop;

/* Exported so the fsck thread can communicated with the kernel. */
#define cachefd 3

/* Inline functions for getting byte-offsets into cull_index and cull_atimes */
__attribute__((pure)) static inline size_t
foffset(slot_t slot,
	unsigned pagesize,
	unsigned perpage,
	unsigned entsize)
{
	div_t offset = div(slot, perpage);
	return (offset.quot * pagesize) + (offset.rem * entsize);
}

__attribute__((pure)) static inline size_t
fpageno(slot_t slot, unsigned perpage)
{
	return slot / perpage;
}


#endif
