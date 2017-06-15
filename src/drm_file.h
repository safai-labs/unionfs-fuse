/*
 * @file data-range map file APIs
 */
#ifndef DRM_FILE_H
#define DRM_FILE_H

#include <sys/types.h>

struct drmf_entry {
	off_t offset;
	size_t len;
};

int drmf_create(const char *mpath, off_t size_initial);
int drmf_destroy(const char *mpath);
int drmf_rename(const char *oldpath, const char *newpath);

int drmf_open(const char *mpath, int *map_fd);
int drmf_close(int map_fd);

int drmf_add_entry(int map_fd, off_t offset, size_t len);
int drmf_get_entries(int map_fd, off_t offset, size_t len,
	struct drmf_entry **entries, unsigned int *count);

int drmf_trunc(int map_fd, off_t new_size);

#endif
