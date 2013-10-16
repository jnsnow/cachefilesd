/**
 * @file fsck.h
 * @author John Huston
 */
#ifndef __VERIFY_H
#define __VERIFY_H

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include "cachefilesd.h"
#include "xattr.h"

/* xattr attribute names. */
#define index_xattr "user.CacheFiles.cull_index" /**< The cull_index xattr name. */
#define atime_xattr "user.CacheFiles.atime_base" /**< The cull_atimes xattr name. */
#define slot_xattr "user.CacheFiles.cache"	 /**< Cache object xattr name. */


/**
 * Represents the state of a particular cache.
 * Includes statistics about the cache indices,
 * used in cache verification and culling queue building.
 */
typedef struct cachefilesd_state {

	size_t index_size;	  /**< size of the cull_index file. */
	size_t atime_size;	  /**< size of the cull_atimes file. */
	unsigned long atime_base; /**< What's the relative atime base of the atimes file? */
	unsigned int ent_size;	  /**< What's the size of an entity in the cull_index file? */
	unsigned int pagesize;	  /**< What's the size of one page of memory? */
	unsigned int num_indices; /**< How many indices do we have in the cull_index file? */
	unsigned int num_atimes;  /**< How many indices do we have in the cull_atimes file? */
	unsigned int num_perpage; /**< How many entities can we fit per-page? */

	const char *rootdir;	  /**< path to the root of the cache */
	char *indexfile;	  /**< path to the cull_index file */
	char *atimefile;	  /**< path to the cull_atimes file */

	struct scan_state *scan;  /**< Scanning state, to be used for a deep scan. */

	bool init;		  /**< Has this structure been initialized? */
	bool read;		  /**< Have we successfully read the state yet? */
	bool bound;		  /**< Have we bound to the cache yet? */
	bool need_fsck;		  /**< Do we need to check index validity? */
	bool fsck_running;	  /**< Are we running an index check right now? */
} cstate;


/**
 * The format of an entity in the culling index.
 * It is similar to a struct file_handle,
 * except we use uint8_t instead of uint32_t,
 * and we pack this record tightly for disk storage.
 */
struct index_record {
	uint8_t len;   /**< Size in bytes of the fh array only. */
	uint8_t type;  /**< The type of file referenced. */
	uint8_t fh[0]; /**< The file handle data. */
} __attribute__ ((packed));


/**
 * The format of the xattr stored on a cache object.
 */
struct cache_xattr {
	slot_t cullslot;  /**< Which cull slot the object is associated with. */
	uint8_t type;	  /**< The type of file (@see struct index_record) */
	uint8_t data[];	  /**< The remaining xattr data, including file handle.) */
} __attribute__ ((packed));


/**
 * An extension of generic_xattr to include the cache_xattr type specifically.
 */
struct cache_xattr_usr {
	struct generic_xattr xattr;	/**< File Descriptor, Buffer Length, then Data [as below] */
	struct cache_xattr cachedata;	/**< Cullslot, File Type, then remaining xattr data. */
} __attribute__ ((packed));


/**
 * scan_state represents a bookmark that keeps
 * place of where we are in an index scan operation.
 */
struct scan_state {
	bool duplicate_pass;	/**< Is this our second pass? */
	bool indices_open;	/**< Are the indices open? */
	bool page_loaded;	/**< Do we have a page/mmap open? */
	bool dirty;		/**< Are our buffers dirty? */

	size_t pageno;		/**< What page number are we on? */
	slot_t index;		/**< What slot number are we looking at? */
	slot_t local_index;	/**< What slot is this relative to the page? */

	size_t fixes;		/**< How many remedial actions did we apply? */
	size_t loads;		/**< How many pageloads/mmaps did we do? */

	struct index_record *r; /**< Pointer to the entity referenced by index */

#ifndef BUFFER
	int index_fd;		/**< File descriptor for culling index */
	int atime_fd;		/**< File descriptor for atimes index */
	size_t map_delta;	/**< For atime mmap: distance to nearest page boundary. */
#else
	FILE *indexfh;		/**< open FILE structure for the index */
	FILE *atimefh;		/**< open FILE structure for the atimes */
#endif
	unsigned char *buffer;	/**< Pointer to a page's worth of entities. */
	atime_t *abuffer;	/**< Pointer to a page's worth of atimes. */
};


/* Exported Functions */
int  cachefilesd_fsck_light(const char *cacheroot, struct cachefilesd_state **s);
int  cachefilesd_fsck_deep(struct cachefilesd_state *s, bool fork);
void state_destroy(struct cachefilesd_state **state);

#endif
