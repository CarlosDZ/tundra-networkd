#ifndef TUNDRA_FILES_H
#define TUNDRA_FILES_H

#include <stddef.h>
#include <sys/types.h>

int files_make_dirs(const char *dir, mode_t mode);
int files_write_atomic(const char *path, const char *data, size_t len,
                       mode_t mode);

#endif
