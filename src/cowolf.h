/*
* License: BSD-style license
* Copyright: Radek Podgorny <radek@podgorny.cz>,
*            Bernd Schubert <bernd-schubert@gmx.de>
*/

#ifndef COWOLF_H
#define COWOLF_H

#include <sys/stat.h>

struct cwf_info {
	int cwf_on;
	int lower_fd;
	int drmap_fd;
};

#define CWF_INFO_INITIALIZER  { 0, -1, -1 }
#define CWF_ON(cw)    ((cw).cwf_on)

int cowolf_create_datamap(const char *path, int branch, off_t file_size);
int cowolf_destroy_datamap(const char *path, int branch);
int cowolf_rename_datamap(const char *oldpath, const char *newpath, int branch);
int cowolf_truncate_datamap(const char *path, int branch, off_t new_size);

int cowolf_read(int upper_fd, struct cwf_info *cw,
	char *buf, size_t size, off_t offset);
int cowolf_write(struct cwf_info *cw, size_t size, off_t offset);

int cowolf_open(const char *path, int branch, int flags, struct cwf_info *cw);
int cowolf_close(struct cwf_info *cw);

#endif
