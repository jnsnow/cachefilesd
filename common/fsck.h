#ifndef __VERIFY_H
#define __VERIFY_H

#include <stdint.h>
#include <stdio.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include "cachefilesd.h"

#define DELETE_EMPTY_DIRS (0)

/* xattr attribute names. */
#define index_xattr "user.CacheFiles.cull_index" /**< The cull_index xattr name. */
#define atime_xattr "user.CacheFiles.atime_base" /**< The cull_atimes xattr name. */
#define slot_xattr "user.CacheFiles.cache"	 /**< Cache object xattr name. */


/**
 * Represents the state of a particular cache.
 * Includes statistics about the cache indices,
 * used in cache verification and culling queue building.
 */
struct cachefilesd_state {

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
	FILE *indexfh;		  /**< open FILE structure for the index */
	FILE *atimefh;		  /**< open FILE structure for the atimes */

	char init;		  /**< Has this structure been initialized? */
	char read;		  /**< Have we successfully read the state yet? */
	char bound;		  /**< Have we bound to the cache yet? */
	char need_fsck;		  /**< Do we need to check index validity? */
	char fsck_running;	  /**< Are we running an index check right now? */
};


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
 * xattr_repair packages information needed to functionalize
 * fixing the xattrs on a given file.
 */
struct xattr_repair {
	struct cache_xattr *xattr; /**< The xattr data to write to the file */
	unsigned long len;	   /**< The length of the xattr data to write */
	int fd;			   /**< The fd of the file to replace the xattrs on. */
};


/**
 * scan_state represents a bookmark that keeps
 * place of where we are in an index scan operation.
 */
struct scan_state {
	short duplicate_pass;	/**< Is this our second pass? */
	size_t pageno;		/**< What page number are we on? */
	slot_t index;		/**< What slot number are we looking at? */
	slot_t local_index;	/**< What slot is this relative to the page? */

	struct index_record *r; /**< Pointer to the entity referenced by index */
	unsigned char *buffer;	/**< Pointer to a page's worth of entities. */
	atime_t *abuffer;	/**< Pointer to a page's worth of atimes. */
};


/**
 * scan_results summarizes the results of a scanning operation.
 */
typedef enum scan_results {
	E_OK,	 /**< Everything is OK, or was repaired successfully. */
	E_DIRTY, /**< Everything was fixed, but we need to write to disk. */
	E_ERROR	 /**< Failed to repair the problem. */
} scan_rc;


/* Exported Functions */
int  cachefilesd_fsck_light(const char *cacheroot, struct cachefilesd_state **s);
int  cachefilesd_fsck_deep(struct cachefilesd_state *s);
int  cachefilesd_fork(struct cachefilesd_state *s);
int  state_init(struct cachefilesd_state **state, const char *root);
void state_destroy(struct cachefilesd_state **state);

#endif
