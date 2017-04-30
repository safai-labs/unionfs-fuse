/**
 * @file drm_mem.c: API implementation for in-memory based data-range map.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>

#include "debug.h"
#include "drm_mem.h"

/* binary search key */
struct rec_srch_key {
	uint64_t key_off;
	const struct drmm_rec *rec_head;
	unsigned int rec_cnt;
};

#define RECSZ sizeof(struct drmm_rec)

#define  MAX(x, y)  (((x) > (y))?(x):(y))
#define  MIN(x, y)  (((x) < (y))?(x):(y))

/**
 * binary search comparison function for finding an element in
 * array which has 'start' offset either smaller than or equal
 * to the offset provided in key.
 */
static
int comp_sml_or_eql(const void *k, const void *e) {

	const struct rec_srch_key *key = (struct rec_srch_key *)k;
	const struct drmm_rec *elem = (struct drmm_rec *)e;
	const struct drmm_rec *last = &key->rec_head[key->rec_cnt-1];

	if (key->key_off == elem->off_start) {
		return 0;
	} else if (key->key_off < elem->off_start) {
		return -1;
	}

	// key->key_off > elem->off_start

	if ((elem == last) || (key->key_off < elem[1].off_start)) {
		// elem is last elem in array or elem-next is larger
		// than elem, then elem is the smaller.
		return 0;
	}

	return 1;
}

/**
 * binary search for finding the element in array that
 *  - either has off_start equal to the offset in key
 *  - or off_start is smaller than the offset in key.
 * that is lower than the offset given in "key".

 * For example, this search returns 'B' for 'offset1',
 * 'A' for 'offset2' and 'D' for 'offset3'.
 *
 *           offset1 -->|
 *  ---------------------------------------------------
 *       |  A  |     | B  |     | C  |       | D |
 *  ---------------------------------------------------
 *     offset2 -->|               offset3 -->|
 */
static
int search_sml_or_eql(off_t key_off, const struct drmm_rec *recs,
	unsigned int count) {

	struct rec_srch_key key = { key_off, recs, count };
	struct drmm_rec *found;
	found = bsearch(&key, recs, count, RECSZ, comp_sml_or_eql);
	if (found == NULL) { return -1; }

	return ((int)(found - recs));
}

/**
 * if "right" is mergable (overlapped or adjacent) with "left", merge to
 * left.
 * @return true if merged, false if not merged.
 */
static
bool rec_merge(struct drmm_rec *left, const struct drmm_rec *right) {

	if ((left->off_end + 1) < right->off_start) {
		return false;
	}

	left->off_end = MAX(right->off_end, left->off_end);
	return true;
}

/**
 * insert a new entry in drmm_rec array. the array buffer must have
 * extra space to add new entry. given entries could be merged after
 * inserting new entry, the array can shrink.
 * @param new_entry  new entry to be inserted.
 * @param recs  drmm_rec array
 * @param num_rec  number of record in the array (before inserting).
 * @return new number of records in array after insertion.
 */
unsigned int drmm_rec_insert(const struct drmm_rec *new_entry,
	struct drmm_rec *recs, unsigned int num_rec) {

	struct drmm_rec *ins_rec;
	struct drmm_rec *last_rec;
	struct drmm_rec *next_rec;
	int pred_indx;

	pred_indx = search_sml_or_eql(new_entry->off_start, recs, num_rec);
	if (pred_indx >= 0) {
		// try to merge with predecessor.
		if (rec_merge(&recs[pred_indx], new_entry)) {
			ins_rec = &recs[pred_indx];
		} else {
			// not merged. add new entry
			int ins_indx = pred_indx + 1;
			unsigned int mv_cnt = num_rec - ins_indx;
			if (mv_cnt > 0) {
				memmove(&recs[ins_indx + 1], &recs[ins_indx],
					mv_cnt*RECSZ);
			}
			recs[ins_indx].off_start = new_entry->off_start;
			recs[ins_indx].off_end = new_entry->off_end;
			num_rec++;
			ins_rec = &recs[ins_indx];
		}
	} else {
		if (num_rec > 0) {
			memmove(&recs[1], recs, num_rec*RECSZ);
		}
		recs[0].off_start = new_entry->off_start;
		recs[0].off_end = new_entry->off_end;
		num_rec++;
		ins_rec = recs;
	}

	last_rec = &recs[num_rec -1];
	next_rec = ins_rec + 1;

	// once new entry is inserted (or merged with predecessor),
        // potentially new inserted entry could be merged with successor
        // elements in array. keep merging it, until merging fails.
	while ((next_rec <= last_rec) && rec_merge(ins_rec, next_rec)) {
		next_rec++;
		num_rec--;
	}

	if ((next_rec - ins_rec) > 1) {
		memmove(ins_rec + 1, next_rec,
			(last_rec - next_rec + 1)*RECSZ);
	}

	RETURN(num_rec);
}

/**
 * truncates the drmm_rec array to new size.
 * @param new_size new trumcated size.
 * @param recs  drmm_rec array
 * @param num_rec  number of record in the array (before truncation).
 * @return new number of records in array after truncation.
 */
unsigned int drmm_rec_truncate(off_t new_size, struct drmm_rec *recs,
	unsigned int num_rec) {

	off_t last_off;
	int indx;

	if ((new_size == 0) || (num_rec == 0)) {
		RETURN(0);
	}

	last_off = new_size -1;
	indx = search_sml_or_eql(last_off, recs, num_rec);
	if (indx < 0) { RETURN(0); };

	recs[indx].off_end = MIN(recs[indx].off_end, last_off);

	RETURN(indx+1);
}

/**
 * find all the overlapped entries within the given <offset, len> range.
 * @param offset  offset range
 * @param len lengh of the range
 * @param recs  drmm_rec array
 * @param num_rec  number of record in the array.
 * @param first_index  pointer where first overlapped entry will be returned.
 * @return number of overlapped entries found.
 */
unsigned int drmm_rec_find_overlaps(off_t offset, size_t len,
	const struct drmm_rec *recs, unsigned int num_rec,
	unsigned int *first_index) {

	off_t range_end;
	unsigned int cnt = 0;
	int indx;
	int i;

	*first_index = 0;

	if ((len == 0) || (num_rec == 0)) {
		RETURN(0);
	}

	range_end = offset + len -1;

	indx = search_sml_or_eql(offset, recs, num_rec);
	if (indx < 0) {
		indx = 0;
	}

	for (i=indx; (i < num_rec) && (recs[i].off_start <= range_end); i++) {
		if (recs[i].off_end < offset) {
			continue;
		}
		if (cnt == 0) {
			*first_index = i;
		}
		cnt++;
	}

	RETURN(cnt);
}

