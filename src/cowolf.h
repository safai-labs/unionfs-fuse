/*
* License: BSD-style license
* Copyright: Radek Podgorny <radek@podgorny.cz>,
*            Bernd Schubert <bernd-schubert@gmx.de>
*/

#ifndef COWOLF_H
#define COWOLF_H

#include <sys/stat.h>

int cowolf_create_datamap(const char *path, int branch, off_t file_size);
int cowolf_destroy_datamap(const char *path, int branch);
int cowolf_rename_datamap(const char *oldpath, const char *newpath, int branch);
int cowolf_truncate_datamap(const char *path, int branch, off_t new_size);

int cowolf_read(int upper_fd, int lower_fd, int map_fd,
	char *buf, size_t size, off_t offset);
int cowolf_write(int map_fd, size_t size, off_t offset);

int cowolf_open(const char *path, int branch, int flags, int *lower_fd,
	int *drmap_fd);
int cowolf_close(int lower_fd, int map_fd);

#endif
