/**
 * @file cowolf.c: Implementation for
 * Copy-On-Write Optimized (for) Large File (COWOLF)
 */
#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/types.h>
#include <inttypes.h>

#include "opts.h"
#include "findbranch.h"
#include "general.h"
#include "cowolf.h"
#include "string.h"
#include "debug.h"
#include "usyslog.h"
#include "drm_file.h"

/**
 * Checks if cowolf could be made ON based on global settings and
 * the size of the file (cowolf is OFF for min threshold size).
 * @return true for ON, false of OFF.
 */
static bool check_cowolfability(off_t file_size) {
	if (uopt.cow_enabled && uopt.cowolf_enabled && (uopt.nbranches == 2)
		&& (file_size >= uopt.cowolf_fsize_th)) {
		RETURN(true);
	}

	RETURN(false);
}

/**
 * Builds the data-range map file path and link path of a filesystem file.
 */
static bool build_cowolf_paths(const char *path, int branch, char *mappath,
	char *linkpath) {

	char metapath[PATHLEN_MAX];
	char tmpbuf[PATHLEN_MAX];
	char *bpath = uopt.branches[branch].path;

	snprintf(tmpbuf, PATHLEN_MAX, "%s%s", path, COWOLF_DRMAPTAG);
	if (BUILD_PATH(metapath, METADIR, tmpbuf))  RETURN(false);
	if (BUILD_PATH(mappath, bpath, metapath)) RETURN(false);

	if (linkpath != NULL) {
		snprintf(tmpbuf, PATHLEN_MAX, "%s%s", path, COWOLF_LINKTAG);
		if (BUILD_PATH(metapath, METADIR, tmpbuf))  RETURN(false);
		if (BUILD_PATH(linkpath, bpath, metapath)) RETURN(false);
	}

	RETURN(true);
}

/**
 * Checks if the file has an associated data-range map.
 */
static bool has_datamap(const char *path, int branch) {

	char mappath[PATHLEN_MAX];

	if (!build_cowolf_paths(path, branch, mappath, NULL)) RETURN(false);

	struct stat buf;
	if (stat(mappath, &buf) == 0) RETURN(true);
	if (errno == ENOENT) RETURN(false);

	RETURN(true);
}

/**
 * Creates data-range map file, provided following conditions are met
 * - COW and COWOLF enabled.
 * - Number of branch is not more than 2.
 * - File is larger than the COWOLF threshold file size.
 * @param path filesystem filepath
 * @param branch branch number
 * @param file_size size of the file
 * @return 0 when datamap is created. -1 when datamap is not created.
 */
int cowolf_create_datamap(const char *path, int branch, off_t file_size) {
        DBG("path = %s, branch = %d, size = %lu\n", path, branch, file_size);

	if (!check_cowolfability(file_size)) {
		RETURN(-1);
	}

	if (create_metapath(path, branch) != 0) {
		USYSLOG(LOG_ERR, "Creating metadata path %s failed.\n", path);
		RETURN(-1);
	}

	char mappath[PATHLEN_MAX];
	char linkpath[PATHLEN_MAX];

	if (!build_cowolf_paths(path, branch, mappath, linkpath)) RETURN(-1);

	if (drmf_create(mappath, file_size) != 0) {
		USYSLOG(LOG_ERR, "Creating drmap %s failed. %s\n",
			mappath, strerror(errno));
		RETURN(-1);
	}

	/**
	 * We create a dummy symlink here to preserve the filepath in
	 * lower RO branch. Given the file may be moved/renamed in top
	 * RW branch, this symlink helps us to track the correct file
	 * in lower branch.
	 */
	unlink(linkpath); // remove previous link - just in case
	if (symlink(path, linkpath) != 0) {
		USYSLOG(LOG_ERR, "Creating symlink %s failed. %s\n",
			linkpath, strerror(errno));
		RETURN(-1);
	}

        RETURN(0);
}

/**
 * Removes a data-range map file (if there is one).
 * @param path filesystem filepath
 * @param branch branch number
 * @return 0 when datamap is removed (or does not exist). -1 when failed
 *         to remove.
 */
int cowolf_destroy_datamap(const char *path, int branch) {
        DBG("path = %s, branch = %d\n", path, branch);

	if (!has_datamap(path, branch)) RETURN(0);

	char mappath[PATHLEN_MAX];
	char linkpath[PATHLEN_MAX];

	if (!build_cowolf_paths(path, branch, mappath, linkpath)) RETURN(-1);
	int res = drmf_destroy(mappath);
	if (res != 0) {
		USYSLOG(LOG_ERR, "Destroying drmap %s failed. %s\n",
			mappath, strerror(errno));
	}

	if ((res = unlink(linkpath)) != 0) {
		USYSLOG(LOG_ERR, "Unlinking %s failed. %s\n",
			linkpath, strerror(errno));
	}

	RETURN(res);
}

/**
 * Truncates the map range in data-range map file.
 * @param path filesystem filepath
 * @param branch branch number
 * @param new_size new size of the file
 * @return 0 when datamap is truncated (or does not exist). -1 when failed
 *         to truncate.
 */
int cowolf_truncate_datamap(const char *path, int branch, off_t new_size) {
        DBG("path = %s, branch = %d, size = %lu\n", path, branch, new_size);

	if (!has_datamap(path, branch)) RETURN(0);

	char mappath[PATHLEN_MAX];
	if (!build_cowolf_paths(path, branch, mappath, NULL)) RETURN(-1);

	int map_fd = -1;
        if (drmf_open(mappath, &map_fd) != 0) {
                RETURN(-1);
        }

	int res = drmf_trunc(map_fd, new_size);
	if (res != 0) {
		USYSLOG(LOG_ERR, "Truncating drmap %s failed. %s\n",
			mappath, strerror(errno));
	}

        drmf_close(map_fd);

	RETURN(res);
}

/**
 * Renames data-range map file.
 * @param oldpath old path for file
 * @param newpath new path for file
 * @param branch branch number
 * @return 0 when datamap is renamed (or does not exist). -1 when failed
 *         to rename.
 */
int cowolf_rename_datamap(const char *oldpath, const char *newpath, int branch) {
        DBG("from %s to %s. branch = %d\n", oldpath, newpath, branch);

	if (!has_datamap(oldpath, branch)) RETURN(0);

	char old_mappath[PATHLEN_MAX];
	char new_mappath[PATHLEN_MAX];
	char old_linkpath[PATHLEN_MAX];
	char new_linkpath[PATHLEN_MAX];

	if (!build_cowolf_paths(oldpath, branch, old_mappath, old_linkpath))
		RETURN(-1);
	if (!build_cowolf_paths(newpath, branch, new_mappath, new_linkpath))
		RETURN(-1);

        if (create_metapath(newpath, branch) != 0) {
		USYSLOG(LOG_ERR, "Creating metadata path %s failed.\n",
			newpath);
		RETURN(-1);
	}

	if (drmf_rename(old_mappath, new_mappath) != 0) {
		USYSLOG(LOG_ERR, "Renaming drmap from %s to %s failed. %s\n",
			old_mappath, new_mappath, strerror(errno));
		RETURN(-1);
	}

	/**
	 * We rename the symlink here. The target in the symlink remains
	 * same - which is the filepath in lower RO branch.
	 */
	if (rename(old_linkpath, new_linkpath) != 0) {
		USYSLOG(LOG_ERR, "Renaming linkfile from %s to %s failed. %s\n",
			old_linkpath, new_linkpath, strerror(errno));
		RETURN(-1);
	}

	RETURN(0);
}

/**
 * Opens files (file in lower branch and associated data-range map file)
 * required for cowolf feature.
 *
 * Here we do couple of checks to make sure that cowolf is ON for a file.
 * (1) Lower branches do not have data-range map file. If there is a
 *     data-range map file (due to past mounting/unmounting activities) in
 *     lower branches, this file cannot be read/written reliably (because
 *     the file here could be sparse). So this function returns error in
 *     such circumstances.
 * (2) If top branch does not have data-range map file, it means this is
 *     not a sparse file (i.e. it has complete data). In that case we can
 *     read data from this branch itself. This function returns SUCCESS
 *     without opening lower branch file and map file (because we do not
 *     need them).
 * (3) If top branch has data-range map file, this function opens lower
 *     branch file and data-range map file (because we cannot relibly
 *     read/write without these files).
 * NOTE: Here we do not check if cowolf feature is enabled or not, because
 * this flag only indicates that this feature is enabled/disabled in
 * current mount. A sparse file and associated data-range map file might
 * exist from past mounting/unmounting operations. If data-range map file
 * exists in top branch, we open it anyway.
 * @param path file path
 * @param branch branch number
 * @param flags flags used for opening the file
 * @param cw cowolf file info
 * @param lower_fd pointer where fd for data-range map file to be returned
 * @return 0 on success, -1 on failure.
 */
int cowolf_open(const char *path, int branch, int flags, struct cwf_info *cw)
{
        DBG("path = %s, branch = %d flags = %x\n", path, branch, flags);

	cw->cwf_on = 0;
	cw->lower_fd = -1;
	cw->drmap_fd = -1;

	int map_fd = -1;
	int lfd = -1;

	if (branch > 0) {
		if (has_datamap(path, branch)) {
			/* lower level must not have datamap */
			USYSLOG(LOG_ERR,
				"Datamap (%s, %d) exists in lower branch.\n",
				path, branch);
			errno = EIO;
			RETURN(-1);
		}

		RETURN(0);
	}

	/* remaining code in this function is only for branch == 0 */

	char mappath[PATHLEN_MAX];
	char linkpath[PATHLEN_MAX];
	if (!build_cowolf_paths(path, branch, mappath, linkpath)) {
		USYSLOG(LOG_ERR, "Datamap (%s) in top branch failed.\n",
				path);
		errno = ENAMETOOLONG;
		USYSLOG(LOG_ERR, "Path (%s) is too long.\n", path);
		RETURN(-1);
	}

	int err = drmf_open(mappath, &map_fd);
	if (err == ENOENT) {
		/* if file is located in top branch and there is no datamap,
		 * it is not a sparse file. simply return success.
		 */
		RETURN(0);
	} else if( err != 0) {
		USYSLOG(LOG_ERR, "Opening datamap (%s) failed.\n", mappath);
		errno = err;
		goto error_out;
	}

	/* we came here because top branch is sparse (which means
	 * we need backend file in lower branch).
	 * hence, open the file from lower branch too.
	 * NOTE: we read the filename from symlink (created at the time
	 * creating datamap), because file might have renamed in top RW
	 * branch.
	 */
	char link_tgt[PATHLEN_MAX];
	int tgt_name_len = readlink(linkpath, link_tgt, PATHLEN_MAX -1);
	if (tgt_name_len < 0) {
		USYSLOG(LOG_ERR, "Readlink (%s) failed. %s\n",
		linkpath, strerror(errno));
		goto error_out;
	}
	link_tgt[tgt_name_len] = 0; // readlink does not put null byte at the end.

	char backpath[PATHLEN_MAX];
	if (BUILD_PATH(backpath, uopt.branches[branch+1].path, link_tgt)) {
		errno = ENAMETOOLONG;
		USYSLOG(LOG_ERR, "Path (%s) is too long.\n", link_tgt);
		goto error_out;
	}

	lfd = open(backpath, flags);
        if (lfd < 0) {
		USYSLOG(LOG_ERR, "Open (%s) failed. %s\n",
			backpath, strerror(errno));
		goto error_out;
	} 

	cw->drmap_fd = map_fd;
	cw->lower_fd = lfd;
	cw->cwf_on = 1;
	RETURN(0);
error_out:
	if (map_fd >= 0) drmf_close(map_fd);
	if (lfd >= 0) close(lfd);

	RETURN(-1);
}

/**
 * Closes the file descriptors for data-range map file and lower branch
 * file (if fds are valid).
 * @param cw cowolf file info
 * @return 0 on success, -1 on failure.
 */
int cowolf_close(struct cwf_info *cw) {
        DBG("lower fd = %d, map_fd = %d\n", cw->lower_fd, cw->drmap_fd);

	if (!cw->cwf_on) RETURN(0);

	if (cw->lower_fd >= 0) {
		close(cw->lower_fd);
	}
	if (cw->drmap_fd >= 0) {
		drmf_close(cw->drmap_fd);
	}

	RETURN(0);
}

/**
 * Reads valid written data (ranges available in data-range map file)
 * from top branch and non-mapped data (area with holes in top branch)
 * from lower branch.
 * Essentially this is scatter-gather read from two different
 * files - one from lower branch and another from top branch.
 * @param upper_fd file descriptor for top branch file sparse file)
 * @param cw cowolf file info
 * @param buf buffer pointer
 * @param size bytes to read
 * @param offset offset in file
 * @return number of bytes read, or -1 on failure.
 */
int cowolf_read(int upper_fd, struct cwf_info *cw,
	char *buf, size_t size, off_t offset) {

	DBG("upper = %d, lower = %d, map = %d, size = %lu, off = %lu\n",
		upper_fd, cw->lower_fd, cw->drmap_fd, size, offset);

	struct drmf_entry *map = NULL;
	unsigned int mcnt = 0;
	if (drmf_get_entries(cw->drmap_fd, offset, size, &map, &mcnt) != 0) {
		USYSLOG(LOG_ERR, "Failed to obtain datamap. fd = %d\n", cw->drmap_fd);
		errno = EIO;
		RETURN(-1);
	}

	off_t start = offset;
	size_t remain = size;
	struct drmf_entry *mpe = map;
	int rval;
	int err_saved = 0;

	while (remain > 0) {

		size_t lower_sz;
		size_t upper_sz;

		if (mpe == NULL) {
			lower_sz = remain;
			upper_sz = 0;
		} else {
			lower_sz = mpe->offset - start;
			upper_sz = mpe->len;
		}

		if (lower_sz > 0) {
			/* read the hole from lower branch */
			rval = pread(cw->lower_fd, buf + (size - remain),
				lower_sz, start);
			if (rval < 0) {
				err_saved = errno;
				break;
			}

			start += rval;
			remain -= rval;

			if (rval < lower_sz) {
				break;
			}
		}

		if (upper_sz > 0) {
			/* read data segment from top branch */
			rval = pread(upper_fd, buf + (size - remain),
				upper_sz, start);
			if (rval < 0) {
				err_saved = errno;
				break;
			}

			start += rval;
			remain -= rval;

			if (rval < upper_sz) {
				break;
			}
		}

		mpe = (mpe < (map + mcnt -1)) ? mpe+1:NULL;
	} // while

	if (map != NULL) free(map);

	if (err_saved > 0) {
		errno = err_saved;
		RETURN(-1);
	}

	RETURN((int)(size - remain));
}

/**
 * Writes data-range maps in map file.
 * NOTE: This function does not write the data itself, but writes
 * data-range map for already-written data.
 * @param cw cowolf file info
 * @param size bytes written
 * @param offset file offset where data written
 * @return 0 if map is written succesfully in file, or -1 if failed.
 */
int cowolf_write(struct cwf_info *cw, size_t size, off_t offset) {

	DBG("map = %d, size = %lu, off = %lu\n", cw->drmap_fd, size, offset);

	if (drmf_add_entry(cw->drmap_fd, offset, size) != 0) {
		USYSLOG(LOG_ERR, "Failed to add datamap. fd = %d\n",
			cw->drmap_fd);
		errno = EIO;
		RETURN(-1);
	}

	RETURN(0);
}

