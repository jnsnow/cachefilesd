#ifndef __CULL_H
#define __CULL_H

#include "cachefilesd.h"
#include "fsck.h"
#include <stdlib.h>

struct pair {
	slot_t	slot;
	atime_t atime;
};

/**
 * Each queue is associated with a cache state.
 * This queue is a "cache" of the most likely culling candidates.
 *
 * Notes:
 * (1) The youngest, oldest and size variables are all unsigned.
 *     However, we frequently use (-1) as a sentinel for youngest
 *     to indicate that the queue is empty. The math is consistent,
 *     but care needs to be taken when writing loops.
 *     This also means we cannot specify a queue size of UINT_MAX,
 *     but this is algorithmically a terrible idea anyway.
 *
 * (2) As an optimization, atimes of time = 0 are considered invalid.
 *     However, we never actually check for this specific value.
 *     Instead, abusing the unsigned nature, we subtract one second from
 *     all incoming atimes. Thus, invalid atimes become UINT_MAX
 *     and naturally get pushed out of the culling queue as being very young.
 *     This speeds the reading of "sparse" indices dramatically as it causes
 *     less branch prediction failures in a very tight loop when reading
 *     millions of atimes.
 *
 * (3) oldest is always set to zero under normal operation, except
 *     during the culling phase, when it might increment up to this->size.
 *     It is always shifted back to zero when the queue is refreshed.
 */
struct queue {
	int ready;
	unsigned youngest;
	unsigned oldest;
	unsigned size;
	unsigned thrash;
	struct pair *queue;
	struct cachefilesd_state *state;

	void (* ins)(struct queue *, atime_t, slot_t);
};

void build_cull_queue(struct queue *cullq, char randomize);
struct queue *new_queue(unsigned exponent, struct cachefilesd_state *state);
void delete_queue(struct queue *cullq);
size_t cull_objects(struct queue *cullq);
size_t queue_refresh(struct queue *cullq);

extern size_t percent_evicted;

/* Some branch prediction stuff to help with reading the indices quickly */
#define likely(x)	__builtin_expect((x),1)
#define unlikely(x)	__builtin_expect((x),0)

#endif
