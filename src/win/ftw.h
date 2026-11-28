// nftw subset: FTW_DEPTH walk used to remove temporary trees.
#pragma once
struct twinc_stat;
struct FTW { int base, level; };
#define FTW_F 0
#define FTW_D 1
#define FTW_DP 5
#define FTW_PHYS 1
#define FTW_DEPTH 8
int nftw(const char* path, int (*fn)(const char*, const struct twinc_stat*, int, struct FTW*), int fds, int flags);
