/*
 * @file drm_mem.h: In-memory data-range map APIs.
*/

#ifndef DRM_MEM_H
#define DRM_MEM_H

#include <stdint.h>
#include <sys/types.h>

struct drmm_rec {
        uint64_t off_start;
        uint64_t off_end;
};

unsigned int drmm_rec_insert(const struct drmm_rec *new_pst,
        struct drmm_rec *recs, unsigned int num_rec);

unsigned int drmm_rec_truncate(off_t new_size, struct drmm_rec *recs,
	unsigned int num_rec);

unsigned int drmm_rec_find_overlaps(off_t offset, size_t len,
        const struct drmm_rec *recs, unsigned int num_rec,
        unsigned int *first_index);

#endif
