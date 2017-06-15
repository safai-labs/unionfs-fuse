/**
 * @file drm_file.c: API implementation for data-range map file.
 *
 * Data-range map file maintains an array of mapped data zones.
 * For example, [100, 249] indicates a mapped zone of 150 bytes
 * (from 100th byte offset to 249th byte offset).
 * The last record includes the region EOF to UINT64_MAX.
 * For example, if the file size is 500 bytes and there is no
 * mapped data zone in file (i.e. while file is sparse), map file
 * is going to have only one entry:
 * rec[0] = [500, UINT64_MAX]
 *
 * A map entry is added (50 bytes from offset 200):
 * rec[0] = [200, 249]
 * rec[1] = [500, UINT64_MAX]
 *
 * Another map entry added (50 byte from offset 450).
 * This time map entry gets merged with the last record:
 * rec[0] = [200, 249]
 * rec[1] = [450, UINT64_MAX]
 *
 * Appending data in file: 100 byte mapped data from offset 500 is
 * added (nothing changes in the mapped record this time):
 * rec[0] = [200, 249]
 * rec[1] = [450, UINT64_MAX]
 *
 * The rational behind having [EOF, UINT64_MAX] as the last entry
 * is to read data from this area from upper branch, instead of
 * trying to read from lower branch (because lower branch is not
 * going to data for this area). For example, when a file grows
 * in upper branch, we must read data from growth area from upper
 * branch. For example, let's assume initial size of a file is 1000
 * bytes, then following operations are done:
 *
 * lseek(fd, 2000, SEEK_SET);
 * write(fd, data, 500);
 * lseek(fd, 1500, SEEK_SET);
 * read(fd, buf, 100);
 *
 * Given this file has the last map record [1000, UINT64_MAX],
 * read() is going to read from upper branch.
 *
 * The last record [EOF, UINT64_MAX] covers truncate related
 * usecases as well. In following example with file initial size
 * 1000 bytes, read() is going to read from upper branch
 * (instead of erroneously reading from lower branch):
 *
 * ftruncate(fd, 400);
 * ftruncate(fd, 3000);
 * lseek(fd, 600, SEEK_SET);
 * read(fd, buf, 100);
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>

#include "usyslog.h"
#include "debug.h"
#include "drm_file.h"
#include "drm_mem.h"

#define RECSZ sizeof(struct drmm_rec)
#define  MAX(x, y)  (((x) > (y))?(x):(y))
#define  MIN(x, y)  (((x) < (y))?(x):(y))

/**
 * lock file
 */
static int file_lock(int fd) {
	struct flock lock;

	memset(&lock, 0, sizeof(lock));
	lock.l_type = F_WRLCK;
	if (fcntl(fd, F_SETLKW, &lock) != 0) {
		USYSLOG(LOG_ERR, "fcntl(%d) failed to lock %s\n",
			fd, strerror(errno));
		RETURN(-1);
	}

	RETURN(0);
}

/**
 * unlock file
 */
static int file_unlock(int fd) {

	struct flock lock;

	memset (&lock, 0, sizeof(lock));
	lock.l_type = F_UNLCK;
	if (fcntl(fd, F_SETLKW, &lock) != 0) {
		USYSLOG(LOG_ERR, "fcntl(%d) failed to unlock %s\n",
			fd, strerror(errno));
		RETURN(-1);
	}

	RETURN(0);
}

/**
 * Loads all data-range map records from file and return them in a
 * allocated buffer. It is caller's responsibility to free the buffer.
 */
static int file_load(int fd, bool extra_rec_space, struct drmm_rec **recs,
	unsigned int *cnt) {

	struct stat fst;
	struct drmm_rec *buf = NULL;

	if (fstat(fd, &fst) != 0) {
		USYSLOG(LOG_ERR, "fstat(%d) failed. %s\n",
			fd, strerror(errno));
		goto error_out;
	}

	if (fst.st_size%RECSZ) {
		/* file is corrupted */
		USYSLOG(LOG_ERR, "bad file size %lu. fd %d\n",
			fst.st_size, fd);
		goto error_out;
	}

	unsigned int num_rec = fst.st_size/RECSZ;

	/* one extra space for new record */
	buf = (struct drmm_rec *) malloc(
			fst.st_size + (extra_rec_space? RECSZ : 0));
	if (buf == NULL) {
		USYSLOG(LOG_ERR, "malloc(%lu) failed.\n",
			fst.st_size + (extra_rec_space? RECSZ : 0));
		goto error_out;
	}

	int sz = pread(fd, buf, fst.st_size, 0);
	if (sz != fst.st_size) {
		USYSLOG(LOG_ERR, "pread(%d, %lu) returned %d. %s\n",
			fd, fst.st_size, sz, strerror(errno));
		goto error_out;
	}

	/* last record off_end is always UINT64_MAX */
	if (buf[num_rec -1].off_end != UINT64_MAX) {

		USYSLOG(LOG_ERR, "fd %d is missing end sentinel rec.\n"
			" got %lu %lu instead.\n",
			fd, buf[num_rec -1].off_start,
			buf[num_rec -1].off_end);
		goto error_out;
	}

	*recs = (struct drmm_rec*) buf;
	*cnt = num_rec;

	RETURN(0);
error_out:
	if (buf != NULL) free(buf);
	RETURN(-1);
}

/**
 * Saves data-range map records in file.
 */
static int file_save(int fd, const struct drmm_rec *recs,
	unsigned int cnt) {
	size_t sz = cnt*RECSZ;

	/* last record off_end is always UINT64_MAX */
	if (recs[cnt -1].off_end != UINT64_MAX) {

		USYSLOG(LOG_ERR, "missing end sentinel rec whle writing.\n"
			" got %lu %lu instead.\n",
			recs[cnt -1].off_start,
			recs[cnt -1].off_end);
	}

	if (pwrite(fd, recs, sz, 0) != sz) {
		USYSLOG(LOG_ERR, "write(%d) failed. %s\n", fd, strerror(errno));
		RETURN(-1);
	}

	if (ftruncate(fd, sz) != 0) {
		USYSLOG(LOG_ERR, "ftruncate(%d) failed. %s\n", fd, strerror(errno));
		RETURN(-1);
	}

	RETURN(0);
}

/**
 * Creates a new data-range map file.
 * @param path data-range map file to be created
 * @return 0 if created successfully. -1 on failure.
 */
int drmf_create(const char *path, off_t size_initial) {
	DBG("%s\n", path);
	int ret = 0;

	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_TRUNC,
			S_IRUSR | S_IWUSR);
	if (fd < 0) {
		if (errno == EEXIST) {
			/**
			 * Given open() is called with O_CREAT|O_EXCL flags,
			 * EEXIST means someone else is trying to create this
			 * map file at the same time. This is not an error
			 * because here we are trying to do the same thing.
			 * So we return success here.
			 * TODO: some NFS might not support O_EXCL.
			 */
			RETURN(0);
		}
		USYSLOG(LOG_ERR, "open(%s) failed. %s\n", path, strerror(errno));
		RETURN(-1);
	}

	if (file_lock(fd) == 0) {
		struct drmm_rec last_rec = { size_initial, UINT64_MAX };
		int sz = pwrite(fd, &last_rec, RECSZ, 0);
		if (sz != RECSZ) {
			USYSLOG(LOG_ERR, "pwrite(%s, %d) failed. ret = %d. %s\n",
				path, (int)RECSZ, sz, strerror(errno));
			ret = -1;
		}

		file_unlock(fd);
	} else {
		ret = -1;
	}

	if (close(fd) != 0) {
		USYSLOG(LOG_ERR, "close(%s) failed. %s\n", path, strerror(errno));
		ret = -1;
	}

	RETURN(ret);
}

/**
 * Removes a data-range map file
 * @param path data-range map file to be destroyed
 * @return 0 if destroyed successfully. -1 on failure.
 */
int drmf_destroy(const char *path) {
	DBG("%s\n", path);
	if (unlink(path) != 0) {
		USYSLOG(LOG_ERR, "unlink(%s) failed. %s\n", path, strerror(errno));
		RETURN(-1);
	}
	RETURN(0);
}

/**
 * Renames data-range map file
 * @param oldpath old path for data-range map file
 * @param newpath new path for data-range map file
 * @return 0 if renamed successfully. -1 on failure.
 */
int drmf_rename(const char *oldpath, const char *newpath) {
	DBG("from %s to %s\n", oldpath, newpath);
	if (rename(oldpath, newpath) != 0) {
		USYSLOG(LOG_ERR, "rename(%s,%s) failed. %s\n",
			oldpath, newpath, strerror(errno));
		RETURN(-1);
	}
	RETURN(0);
}

/**
 * Opens data-range map file.
 * @param mpath path for data-range map file
 * @param map_fd pointer where fd to be returned
 * @return 0 if opened successfully. -1 on failure.
 */
int drmf_open(const char *mpath, int *map_fd) {
	DBG("%s\n", mpath);
	*map_fd = -1;
	int fd = open(mpath, O_RDWR);
	if (fd < 0) {
		int errsaved = errno;
		USYSLOG(LOG_ERR, "open(%s) failed. %s\n", mpath, strerror(errno));
		RETURN(errsaved);
	}

	/* TODO: check sanity of the file here? */

	*map_fd = fd;
	RETURN(0);
}

/**
 * Closes data-range map file.
 * @param map_fd fd for data-range map file to be closed
 * @return 0 if closed successfully. -1 on failure.
 */
int drmf_close(int map_fd) {
	DBG("%d\n", map_fd);
	if (close(map_fd) != 0) {
		USYSLOG(LOG_ERR, "close(%d) failed. %s\n", map_fd, strerror(errno));
	}
	RETURN(0);
}

/**
 * Adds a new map entry in the data-range map file.
 * @param map_fd map file fd
 * @param offset offset in the file
 * @param len length of map range
 * @return 0 on success, -1 on failure.
 */
int drmf_add_entry(int map_fd, off_t offset, size_t len) {
	DBG("fd = %d, off = %lu, len = %lu\n", map_fd, offset, len);

	struct drmm_rec *recs = NULL;
	int rval = -1;
	unsigned int num_rec = 0;
	struct drmm_rec new_rec = { offset, offset + len -1 };

	if (file_lock(map_fd) != 0) {
		RETURN(-1);
	}

	if (file_load(map_fd, true, &recs, &num_rec) != 0) {
		goto error_out;
	}

	num_rec = drmm_rec_insert(&new_rec, recs, num_rec);

	if (file_save(map_fd, recs, num_rec) != 0) {
		goto error_out;
	}

	rval = 0;
error_out:
	if (recs != NULL) free(recs);
	file_unlock(map_fd);

	RETURN(rval);
}

/**
 * Get data-range map entries of specified <offset, len> range
 * from the map file.
 * @param map_fd map file fd
 * @param offset offset in the file
 * @param len lenght of range for which map is requested
 * @param entries address where allocated map entries to be returned.
 *        NULL if no map entry is available within the range.
 * @param count pointer where number of map entries to be returned.
 * @return 0 on success, -1 on failure.
 */
int drmf_get_entries(int map_fd, off_t offset, size_t len,
	struct drmf_entry **entries, unsigned int *count) {
	DBG("fd = %d, off = %lu, len = %lu\n", map_fd, offset, len);

	struct drmm_rec *recs = NULL;
	unsigned int num_rec = 0;
	unsigned int olap_indx_first = 0;
	struct drmf_entry *dm_tmp = NULL;
	unsigned int i, olap_cnt;
	struct drmm_rec *olap_rec;
	off_t range_st, range_en;

	*entries = NULL;
	*count = 0;

	if (len == 0) {
		RETURN(0);
	}

	if (file_lock(map_fd) != 0) {
		RETURN(-1);
	}

	if (file_load(map_fd, false, &recs, &num_rec) != 0) {
		file_unlock(map_fd);
		RETURN(-1);
	}

	file_unlock(map_fd);

	olap_cnt = drmm_rec_find_overlaps(offset, len, recs, num_rec,
			&olap_indx_first);
	if (olap_cnt == 0) {
		RETURN(0); /* not an error */
	}

	dm_tmp = (struct drmf_entry *)malloc(
			sizeof(struct drmf_entry)*olap_cnt);
	if (dm_tmp == NULL) {
		free(recs);
		RETURN(-1);
	}

	range_st = offset;
	range_en = offset + len -1;

	for (i = 0; i < olap_cnt; i++) {
		olap_rec = &recs[olap_indx_first + i];
		dm_tmp[i].offset = MAX(olap_rec->off_start, range_st);
		dm_tmp[i].len = MIN(olap_rec->off_end,
					range_en) - dm_tmp[i].offset + 1;
	}

	free(recs);

	*entries = dm_tmp;
	*count = olap_cnt;

	RETURN(0);
}

/**
 * Truncates the the data-range map to new size of of a file.
 * All the map records beyond the new_size are removed from the map file.
 * Given the last record also include the region EOF to UINT64_MAX,
 * the last record is adjusted accordingly.
 * @param map_fd file desc of map file
 * @param new_size new size of file
 * @return 0 is truncated successfully (including no record removed).
 *         - on failure.
 */
int drmf_trunc(int map_fd, off_t new_size) {
	DBG("map_fd = %d, size = %lu\n", map_fd, new_size);

	struct drmm_rec *recs = NULL;
	unsigned int num_rec = 0;
	int rval = -1;
	int lck_ret = -1;
	off_t saved_last_start;

	if ((lck_ret = file_lock(map_fd)) != 0) {
		goto err_out;
	}

	if (file_load(map_fd, true, &recs, &num_rec) != 0) {
		goto err_out;
	}

	/* last record is for the region beyond EOF, truncate
	 * without it.
	 */
	saved_last_start = recs[num_rec -1].off_start;
	num_rec = drmm_rec_truncate(new_size, recs, num_rec -1);

	/* add last record:
	 * CASE 1:
	 * If truncation position is in unmapped area, truncated
	 * part simply gets added to the last record.
	 *   rec[0] = [100, 199]
	 *   rec[1] = [500, UINT64_MAX]
	 *    --------------------------------
	 *   |          |MMMMMMMM|            |
	 *   |          |100     |199         |499 (EOF)
	 *    --------------------------------
	 *        truncate here -------->|399
	 *
	 * after truncation:
	 *   rec[0] = [100, 199]
	 *   rec[1] = [400, UINT64_MAX]
	 *    ---------------------------
	 *   |          |MMMMMMMM|       |
	 *   |          |100     |199    |399 (EOF)
	 *    ---------------------------
	 *
	 * CASE 2:
	 * If truncation position is in mapped area, mapped area
	 * gets merged into the last record (which is EOF to UINT64_MAX).
	 *   rec[0] = [100, 299]
	 *   rec[1] = [500, UINT64_MAX]
	 *    ---------------------------------
	 *   |          |MMMMMMMMMMMM|         |
	 *   |          |100         |299      |499 (EOF)
	 *    ---------------------------------
	 *    truncate here -->|199
	 *
	 * after truncation:
	 *   rec[0] = [100, UINT64_MAX]
	 *    -----------------
	 *   |          |MMMMMM|
	 *   |          |100   |199 (EOF)
	 *    -----------------
	 */
	if (saved_last_start < new_size) {
		new_size = saved_last_start;
	}
	struct drmm_rec last_rec = { new_size, UINT64_MAX };
	num_rec = drmm_rec_insert(&last_rec, recs, num_rec);

	if (file_save(map_fd, recs, num_rec) != 0) {
		goto err_out;
	}

	rval = 0;
err_out:
	if (recs != NULL) free(recs);
	if (lck_ret == 0) file_unlock(map_fd);

	RETURN(rval);
}

