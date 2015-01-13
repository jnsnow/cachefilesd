#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <sys/time.h>
#include <sys/stat.h>

#include "cachefilesd.h"
#include "cull.h"
#include "debug.h"

/******************************************************************************/

#define PAIR(Q,N) ((Q)->queue[(N)])
#define YOUNGEST(Q) ((Q)->queue[(Q)->youngest])
#define OLDEST(Q) ((Q)->queue[(Q)->oldest])

#define TIME(Q,N) (PAIR((Q),(N)).atime)
#define SLOT(Q,N) (PAIR((Q),(N)).slot)

#define QSIZE(Q) ((Q)->youngest - (Q)->oldest + 1)

void cull_file(const char *filename);
static int cull_slot(slot_t slot);
static char in_queue(struct queue *cullq, slot_t slot, atime_t atime, unsigned i);

#ifdef FORCE_EVICT
/* Debugging values. */
size_t percent_evicted = 0;
#endif


static void insert_nonfull(struct queue *cullq, slot_t slot, atime_t atime);
static void insert_full(struct queue *cullq, slot_t slot, atime_t atime);
static void insert_into_cull_table(struct queue *cullq, slot_t slot, atime_t atime);

bool queue_write(struct queue *cullq, const char *filename);

/******************************************************************************/


/**
 * Update a particular pair with new values.
 * @param p A pointer to the pair to update.
 * @param slot The slot number to store.
 * @param atime The atime to store.
 */
static inline void pair_set(struct pair *p, slot_t slot, atime_t atime)
{
	p->slot = slot;
	p->atime = atime;
}


/**
 * Returns a left-justified index of this item's correct
 * position in the queue. This means it can be 0, or even
 * past the end if the queue is full.
 * @param cullq The queue to determine an insertion point for.
 * @param atime The atime of the item to insert.
 */
static inline unsigned get_insert(struct queue *cullq, atime_t atime)
{
	unsigned l = 0;
	unsigned r = cullq->youngest;
	unsigned m;

	/* Older than the oldest item (or empty queue.) */
	if (cullq->youngest == -1 ||
	    atime <= OLDEST(cullq).atime) return 0;

	/* Younger than the youngest item, add to end */
	if (atime >= YOUNGEST(cullq).atime) return cullq->youngest + 1;

	/* Find insertion point. */
	do {
		m = (l + r) / 2;
		if (atime > TIME(cullq,m))
			l = m + 1;
		else
			r = m;
	} while (r > l);

	return l;
}


/**
 * Compares two pair's atimes.
 * @return 0 if they're equal, 1 if "a > b", -1 otherwise.
 */
static inline int pair_cmp(void const *a, void const *b)
{
#define timeof(X) (((struct pair *)X)->atime)
	if (timeof(a) == timeof(b)) return 0;
	return (timeof(a) > timeof(b)) ? 1 : -1;
}


#ifdef CONST_CHECK
/**
 * Debug method. Ensures the validity of the queue ordering.
 * @param cullq The queue to check the consistency of.
 * @return 0 for OK, -1 otherwise.
 */
static char check_consistency(struct queue *cullq)
{
	unsigned i;

	if (cullq->youngest == -1) return 0;
	if (QSIZE(cullq) < 2) return 0;

	for (i = cullq->oldest + 1; i <= cullq->youngest; ++i) {
		if (TIME(cullq,i) < TIME(cullq,i-1)) {
			debug(0, "PROBLEM: [%u,%u] after [%u,%u]\n",
			      SLOT(cullq,i),
			      TIME(cullq,i),
			      SLOT(cullq,i-1),
			      TIME(cullq,i-1));
			return -1;
		}
	}
	return 0;
}
#endif

/**
 * Insert a new slot+atime pair into a "fresh" table.
 *
 * @param cullq The queue to insert into
 * @param slot The slot number of the new item
 * @param atime The atime associated with this slot
 */
static void insert_nonfull(struct queue *cullq,
			   slot_t slot,
			   atime_t atime)
{
	unsigned i;

	i = cullq->youngest + 1;
	pair_set(&PAIR(cullq,i), slot, atime);
	cullq->youngest++;

	if (cullq->youngest == cullq->size - 1) {
	  cullq->oldest = 0;
	  /* cullq->ins = insert_full; */
	  /* update which insert function to use, now */
	  qsort(&OLDEST(cullq), (cullq->youngest - cullq->oldest) + 1,
		sizeof(struct pair), pair_cmp);
	}

	return;
}

/**
 * Insert a new slot+atime pair into a fresh, but already full, table.
 *
 * @param cullq The queue to insert into
 * @param slot The slot number of the new item
 * @param atime The atime associated with this slot
 */
static void insert_full(struct queue *cullq,
			slot_t slot,
			atime_t atime)
{
	unsigned i;

	if (atime >= YOUNGEST(cullq).atime) return;
	cullq->youngest--;

	/* get insertion point. oldest and newest objects are fast searches O(1) */
	i = get_insert(cullq, atime);

	/* shift the Ith item to I+1, creating a new insertion point at I.
	 * The number of items currently in the table is (youngest + 1),
	 * because 'youngest' is stored as a zero index.
	 * We therefore move (num - i) objects. */
	memmove(&PAIR(cullq,i+1),
		&PAIR(cullq,i),
		((cullq->youngest + 1) - i) * sizeof(PAIR(cullq,0)));

	pair_set(&PAIR(cullq,i), slot, atime);

	cullq->youngest++;

#ifdef CONST_CHECK
	if (check_consistency(cullq)) {
		debug(0,"Failed consistency check, i was %d, atime was %u", i, atime);
		exit(254);
	}
#endif

	return;
}



/**
 * Insert a new slot+atime pair into the table.
 * If the table is full, the table will make room only if
 * the new value is old enough.
 *
 * @param cullq The queue to insert into
 * @param slot The slot number of the new item
 * @param atime The atime associated with this slot
 */
static void insert_into_cull_table(struct queue *cullq,
				   slot_t slot,
				   atime_t atime)
{
	unsigned i;

	/* Is the table full? */
	if (cullq->youngest == cullq->size - 1) {
		if (atime >= YOUNGEST(cullq).atime)
			return;
		/* last element gets pushed out */
		cullq->youngest--;
	}

	/* Is the table overfull? ...
	 * We are careful to exclude cullq->youngest = -1. */
	else if (unlikely(cullq->youngest + 1 > cullq->size)) {
		debug(0, "youngest: %u; capacity: %u", cullq->youngest, cullq->size - 1);
		error("Cull table overfull");
	}

	/* get insertion point.
	 * oldest and newest objects are fast searches. */
	i = get_insert(cullq, atime);
	if (in_queue(cullq, slot, atime, i)) return;


	/* shift the Ith item to I+1, creating a new insertion point at I.
	 * The number of items currently in the table is (youngest + 1),
	 * because 'youngest' is stored as a zero index.
	 * We therefore move (num - i) objects. */
	memmove(&PAIR(cullq,i+1),
		&PAIR(cullq,i),
		((cullq->youngest + 1) - i) * sizeof(PAIR(cullq,0)));

	pair_set(&PAIR(cullq,i), slot, atime);

	cullq->youngest++;

#ifdef CONST_CHECK
	if (check_consistency(cullq)) {
		debug(0,"Failed consistency check, i was %d, atime was %u", i, atime);
		exit(254);
	}
#endif

	return;
}


/**
 * Build, or Rebuild, a culling queue based on the atimes file specified
 * by the state structure associated with this culling queue.
 * @param cullq The queue to build up
 * @param randomize Optionally, should we read the source atimes file in a random order?
 */
void build_cull_queue(struct queue *cullq, char randomize)
{
	/* Configuration (filesize, queuesize, readsizes) */
	const size_t readnum = (1 << 12);
	const size_t readbytes = readnum * sizeof(atime_t);
	struct stat st;

	/* Used for the actual reading */
	int fd, n = readnum;
	slot_t slot = 0;
	atime_t *abuff;
	unsigned i, p, chunk, chunks;

	/* For randomization */
	unsigned *readlist = NULL;

	if (cullq->youngest != -1) {
		fprintf(stderr, "skip");
		return;
	}

	if (cullq->oldest)
		error("Inconsistency: build_cull_queue called when the oldest element was not 0.");

	/* Mark as non-ready while we're building ... */
	cullq->ready = 0;

	fd = open(cullq->state->atimefile, O_RDONLY);
	if (fd < 0) {
		oserror("Failed to open atimes file (%s) to build a culling queue.",
			cullq->state->atimefile);
	}

	abuff = (atime_t *)malloc(readbytes);
	if (!abuff) {
		oserror("Failed to allocate space (%lub) for reading %lu atime entries",
			readbytes, readnum);
	}

	fstat(fd, &st);
	chunks = (st.st_size + readbytes - 1) / readbytes;
	if (!chunks) return;


	/* This code randomizes the order of the chunks we read.
	 * It does unfortunately take up some space in memory,
	 * but it helps avoid the worst case buffer-building scenario. */
	if (randomize) {
		unsigned j;
		readlist = (unsigned *)malloc(sizeof(unsigned) * chunks);
		readlist[0] = 0;
		/* inside-out Knuth-Fisher-Yates shuffle:
		 * read these chunks in a random order. */
		for (i = 1; i < chunks; ++i) {
			j = random() % (i+1);
			readlist[i] = readlist[j];
			readlist[j] = i;
		}
	}

	/* initial read */
	{
		p = 0;
		chunk = randomize ? readlist[p] : p;
		lseek(fd, chunk * readbytes, SEEK_SET);
		slot = chunk * readnum;
		n = read(fd, abuff, readbytes) / sizeof(atime_t);
		for ( i = 0; i < n; ++i, ++slot) {
			insert_nonfull(cullq, slot, abuff[i] - 1);
		}
	}

	/* Read the atimes file chunk-by-chunk, either in-order or
	 * a random chunk at a time. So either we use p linearly,
	 * or as an index into our randomized chunk-order list. */
	for (p = 1; p < chunks; ++p) {
		chunk = randomize ? readlist[p] : p;
		lseek(fd, chunk * readbytes, SEEK_SET);
		slot = chunk * readnum;

		n = read(fd, abuff, readbytes) / sizeof(atime_t);
		for (i = 0; i < n; ++i, ++slot) {
			//cullq->ins(cullq, slot, abuff[i] - 1);
			//insert_into_cull_table(cullq, slot, abuff[i] - 1);
			insert_full(cullq, slot, abuff[i] - 1);
		}
	}

	/* Mark as ready! */
	if (cullq->oldest == 0 && cullq->youngest != -1) cullq->ready = 1;
	close(fd);
	free(abuff);
	if (randomize)
		free(readlist);
}


/**
 * Instantiate a new queue associated with a particular cachefilesd cache.
 * @param exponent How large should the queue be? It will be 2^exponent.
 * @param state State information for a particular cachefilesd cache to associate this queue with.
 * @return A pointer to the new queue.
 */
struct queue *new_queue(unsigned exponent, struct cachefilesd_state *state)
{
	struct queue *cullq = calloc(sizeof(struct queue),1);
	if (!cullq) {
		dperror("Failed to allocate memory for queue header");
		return cullq;
	}

	cullq->size = 1 << exponent;

	cullq->queue = calloc(sizeof(struct pair),cullq->size);
	if (!cullq->queue) {
		dperror("Failed to allocate memory for queue");
		free(cullq);
		return NULL;
	}

	cullq->ready = 0;
	cullq->oldest = 0;
	cullq->youngest = -1;
	cullq->thrash = 0;

	/* cullq->ins = insert_into_cull_table; */

	cullq->state = state;
	return cullq;
}


/**
 * Delete a queue. Simply frees memory.
 * @param cullq The queue to delete.
 */
void delete_queue(struct queue *cullq)
{
	free(cullq->queue);
	free(cullq);
}


/**
 * Advance the queue pointer by an arbitrary amount.
 * Optionally, tell valgrind that the previous bits in memory are
 * no longer valid queue slots.
 * @param cullq The queue to advance
 * @param by How many slots we should advance the pointer.
 */
static inline void advance_queue(struct queue *cullq, size_t by)
{
#ifdef _USE_VALGRIND
	VALGRIND_MAKE_MEM_UNDEFINED(&OLDEST(cullq), sizeof(struct pair) * by);
#endif
	cullq->oldest += by;
}


/**
 * Invalidate the entire queue. Optionally, tell valgrind the queue is now invalid.
 * @param cullq the queue to invalidate.
 */
static inline void erase_queue(struct queue *cullq)
{
#ifdef _USE_VALGRIND
	VALGRIND_MAKE_MEM_UNDEFINED(cullq->queue, sizeof(struct pair) * cullq->size);
#endif
	cullq->oldest = 0;
	cullq->youngest = -1;
	cullq->ready = 0;
}

/**
 * cull_objects will mull over the culling queue, find the first valid object, and cull it.
 * We will verify each item in the queue before attempting to cull it.
 * It is possible we will run out of entries before we succeed in culling anything.
 * @param cullq The queue to pick an item from.
 * @return The size of the queue after we were done with it.
 */
size_t cull_objects(struct queue *cullq)
{
	char active, fresh, success = 0;
	slot_t slot;
	atime_t atime, file_atime;

	int cullfd, atimefd, rc;
	size_t offset;

	if (cullq->oldest >= cullq->size)
		error("Cullable object count is inconsistent");

	cullfd = open(cullq->state->indexfile, O_RDONLY);
	if (cullfd < 0) {
		oserror("Failed to open culling index (%s) to verify slot before cull",
			cullq->state->indexfile);
	}

	atimefd = open(cullq->state->atimefile, O_RDONLY);
	if (atimefd < 0) {
		oserror("Failed to open atimes index (%s) to verify slot before cull",
			cullq->state->atimefile);
	}

	/* Make a little note if this was a 'fresh' queue. */
	fresh = (cullq->oldest == 0);

	/* process the queue until we cull one item, or run out of items. */
	do {
		slot = OLDEST(cullq).slot;
		atime = OLDEST(cullq).atime + 1;
		offset = foffset(slot, cullq->state->pagesize,
				 cullq->state->num_perpage,
				 cullq->state->ent_size);
		debug(3, "Considering culling %u", slot);

		/* Is this atime even valid? If not,
		 * that means we've run out of valid entries in the queue.
		 * Because they are sorted off-by-one, a zero entry MUST
		 * mean the end of the queue. */
		if (!atime) {
			debug(3,"Empty atime. Considering queue empty.");
			erase_queue(cullq);
			break;
		}

		/* Seek and read first byte from cullslot to verify validity */
		rc = lseek(cullfd, offset, SEEK_SET);
		if (unlikely(rc != offset)) {
			oserror("Failed to seek to correct slot in culling index");
		}

		rc = read(cullfd, &active, 1);
		if (unlikely(rc != 1)) {
			oserror("Failed to read slot status from culling index");
		}

		if (!active) {
			debug(2, "Entry in cull_index is already gone.");
			advance_queue(cullq, 1);
			continue;
		}

		/* Seek and read atime to verify slot validity */
		rc = lseek(atimefd, sizeof(atime_t) * slot, SEEK_SET);
		if (unlikely(rc != sizeof(atime_t) * slot)) {
			oserror("Failed to seek to this slot's atime when reading file...");
		}

		rc = read(atimefd, &file_atime, sizeof(atime_t));
		if (unlikely(rc != sizeof(atime_t))) {
			oserror("Failed to read atime from file in order to verify slot.");
		}

		if (file_atime != atime) {
			debug(2, "Slot %u was touched since we added it to the queue.",
			      slot);
			advance_queue(cullq, 1);
			continue;
		}

		/* Request the slot be culled. */
		rc = cull_slot(slot);
		advance_queue(cullq, 1);

		/* This slot didn't work, silently roll on to the next one */
		if (rc) continue;
		success = 1;

	} while (cullq->oldest < cullq->size);

	/* Keep track if we are being fruitful in our culling efforts. */
	if (!success && fresh)
		cullq->thrash++;
	else if (success)
		cullq->thrash = 0;

	/* If we've emptied out our queue, notate it as needing to be re-filled. */
	if (cullq->oldest == cullq->size) {
		debug(1, "queue was depleted, marking it empty.");
		erase_queue(cullq);
	}

	close(cullfd);
	close(atimefd);
	return QSIZE(cullq);
}


/**
 * cull_file will cull a file representing an object in the current working directory.
 * - requests CacheFiles rename the object "<cwd>/filename" to the graveyard
 * @param filename The file to cull.
 */
void cull_file(const char *filename)
{
	char buffer[NAME_MAX + 30];
	int ret, n;

	n = sprintf(buffer, "cull %s", filename);

	/* command the module */
	ret = write(cachefd, buffer, n);
	if (ret < 0 && errno != ESTALE && errno != ENOENT && errno != EBUSY)
		oserror("Failed to cull object");
}


/**
 * cull_slot will remove a file from the cache via its index slot number.
 * @param slot The slot to cull.
 */
static int cull_slot(slot_t slot)
{
	char buffer[20];
	int rc, len;

	len = snprintf(buffer, 20, "cullslot %u", slot);
	debug(2, "%s", buffer);
	rc = write(cachefd, buffer, len);
	if (rc < len) {
		rc = errno;
		debug(1, "cmd(%s) failed: %m", buffer);
		return rc;
	}

	return 0;
}


/**
 * in_queue is a best-effort function that detects if an object is
 * already in the culling queue or not. It may return a false negative
 * if the atime associated with the object has changed inbetween its
 * addition to the queue and the verification.
 * @param cullq The queue to search.
 * @param slot The slot number we seek to verify.
 * @param atime That slot's atime.
 * @param i The insertion index of this hypothetical slot.
 * Retrieve this value by using get_insert.
 * @return True/False. True indicates the item was definitely found in the cache.
 */
char in_queue(struct queue *cullq, slot_t slot, atime_t atime, unsigned i)
{
	unsigned j = i;

	/* Ensure the insertion point, i, is a valid array index. */
	if (j >= QSIZE(cullq) || j > cullq->youngest) return 0;
	/* Bullseye. */
	if (SLOT(cullq,j) == slot) return 1;

	/* Might be duplicate atimes, so search to the left and right until
	 * our ship runs ashore, or we find the man we're looking for. */
	/* (A) the +1 lets us use lt instead of lte.
	 * (B) the +1 overflows the -1 sentinel to be 0, and the loop is avoided. */
	while (++j < cullq->youngest + 1 && TIME(cullq,j) == atime)
		if (SLOT(cullq,j) == slot) return 1;
	for (j = i; --j != -1 && TIME(cullq,j) == atime;)
		if (SLOT(cullq,j) == slot) return 1;

	/* Couldn't find it! */
	return 0;
}


/**
 * queue_refresh will freshen the queue with new items,
 * and make sure the existing items are valid.
 * @param cullq the queue to freshen.
 * @return The number of queue items evicted.
 */
size_t queue_refresh(struct queue *cullq)
{
	int fd, rc;
	atime_t new_atime;
	unsigned i, evicted = 0;

#ifdef FORCE_EVICT
	/* For testing, compute an eviction cutoff for simulating ... */
	size_t evict = (((float) percent_evicted / 100.0) * (float)cullq->size);
#endif

	cullq->ready = 0;
	fd = open(cullq->state->atimefile, O_RDONLY);
	if (fd < 0)
		oserror("Failed to open atimes file during queue refresh");

	/* The queue refresh proceeds in three steps. */
	/* Step one: iterate through the queue and refresh the atimes present. */
	for (i = cullq->oldest;
	     i <= cullq->youngest && i < cullq->size;
	     ++i) {

		/* Read the current atime in from file. */
		rc = lseek(fd, sizeof(atime_t) * SLOT(cullq,i), SEEK_SET);
		if (unlikely(rc < 0))
			oserror("Failed to seek to position in atime file (%s)",
				cullq->state->atimefile);

		rc = read(fd, &new_atime, sizeof(atime_t));
		if (unlikely(rc != sizeof(atime_t)))
			oserror("Failed to retrieve atime from file (%s)",
				cullq->state->atimefile);

#ifdef FORCE_EVICT
		/* Debug note: we use 'evict' to simulate a
		 * forced percentage of evictions. */
		if (i < evict) new_atime = 0;
#endif

		/* Update the atime in the queue. */
		if (new_atime - 1 != TIME(cullq,i)) {
			evicted++;
			debug(4,"Freshen: had (%u), updated to (%u)",
			      TIME(cullq,i), new_atime);
		}
		TIME(cullq,i) = new_atime - 1;
	}
	close(fd);

	/* The queue has been previously nibbled at, shift it down. */
	if (cullq->oldest != 0) {
		memmove(&PAIR(cullq,0),
			&OLDEST(cullq),
			sizeof(PAIR(cullq,0)) * (cullq->youngest - cullq->oldest + 1));
		evicted += cullq->oldest;
		cullq->youngest -= cullq->oldest;
		cullq->oldest = 0;
	}

	/* If nothing changed at all, AND the queue is full,
	 * there's nothing further we need to do at all. */
	if (!evicted && cullq->youngest == cullq->size - 1) {
		return 0;
	}

	/* Step Two: Re-sort the queue so that it is at least internally consistent.
	 * We only need to do this if anything changed. */
	else if (evicted) qsort(&OLDEST(cullq), (cullq->youngest - cullq->oldest) + 1,
				sizeof(struct pair), pair_cmp);


#ifdef CONST_CHECK
	if (check_consistency(cullq)) {
		for (i = 0 ; i < cullq->size; ++i) {
			debug(0,"{%8u}", TIME(cullq,i));
		}
		debug(0, "Failed consistency check after qsort");
		exit(10);
	}
#endif

	/* Step 3: Re-read the cull_atimes file and ensure our queue is
	 * accurate. The randomized-read ability of build_cull_queue
	 * is left as an option: it's possible that with a low eviction
	 * rate that setting this to 0 might yield better speeds. */
	//cullq->ins = insert_into_cull_table;
	build_cull_queue(cullq, 1);

	queue_write(cullq, ".cullq.cache");
	return evicted;
}

bool queue_write(struct queue *cullq, const char *filename)
{
	FILE *fh;
	int n;

	fh = fopen(filename, "w+");
	if (!fh) {
		dperror("Could not open %s for writing", filename);
		return false;
	}

	n = fwrite(cullq->queue, sizeof(PAIR(cullq,0)), QSIZE(cullq), fh);
	if (n < QSIZE(cullq)) {
		dperror("Could not empty the entire queue to disk");
		fclose(fh);
		return false;
	}

	fclose(fh);
	return true;
}
