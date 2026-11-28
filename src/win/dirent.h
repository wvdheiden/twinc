// opendir/readdir on top of FindFirstFileW: UTF-8 names and d_type.
#pragma once
#include <string>
#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8
#define DT_LNK 10
struct dirent {
  char d_name[1024];
  unsigned char d_type;
};
typedef struct twinc_DIR DIR;
DIR* opendir(const char* path);
struct dirent* readdir(DIR* d);
int closedir(DIR* d);
