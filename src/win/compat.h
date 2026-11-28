// Windows compatibility layer: lets the POSIX-style code of twinc build with MinGW-w64.
// Paths inside twinc stay UTF-8 with '/' separators ("C:/Users/walter").
#pragma once
#ifdef _WIN32
#include <errno.h>
#include <stdint.h>
#include <string>
#include <time.h>

std::wstring u8to16(const std::string& s);
std::string u16to8(const wchar_t* s, int len = -1);

// --- struct stat with nanosecond times, 64-bit size and Windows attributes.
// "stat" is renamed so both the struct and the functions use our version.
struct twinc_stat {
  unsigned st_mode;
  int64_t st_size;
  struct timespec st_atim, st_mtim, st_ctim;
  time_t st_atime, st_mtime, st_ctime;
  unsigned st_uid, st_gid, st_nlink;
  uint64_t st_dev, st_ino;
  unsigned long st_attr;  // FILE_ATTRIBUTE_*
};
int twinc_stat_fn(const char* path, struct twinc_stat* st);   // follows links
int twinc_lstat_fn(const char* path, struct twinc_stat* st);  // reports links as S_IFLNK
int twinc_fstat_fn(int fd, struct twinc_stat* st);
#define stat twinc_stat
#define lstat twinc_lstat_fn
#define fstat twinc_fstat_fn
// "stat(path, &st)" calls: the macro above turns them into twinc_stat(path,&st); route that call.
inline int twinc_stat(const char* p, struct twinc_stat* s) { return twinc_stat_fn(p, s); }

#ifndef S_IFLNK
#define S_IFLNK 0xA000
#endif
#ifndef S_ISLNK
#define S_ISLNK(m) (((m) & 0xF000) == S_IFLNK)
#endif
#ifndef S_IXUSR
#define S_IXUSR 0100
#endif
#ifndef S_IXGRP
#define S_IXGRP 0010
#define S_IXOTH 0001
#endif
#ifndef S_ISUID
#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000
#endif
#ifndef S_ISSOCK
#define S_ISSOCK(m) 0
#endif
#ifndef S_ISBLK
#define S_ISBLK(m) 0
#endif

int mkdir(const char* p, int mode);  // overload of MinGW's one-argument mkdir
int fchmod(int fd, unsigned mode);
int symlink(const char* target, const char* link);
long readlink(const char* path, char* buf, size_t n);
char* realpath(const char* p, char* resolved);

#ifndef AT_FDCWD
#define AT_FDCWD -100
#define AT_SYMLINK_NOFOLLOW 0x100
#endif
#ifndef UTIME_OMIT
#define UTIME_OMIT ((1L << 30) - 2L)
#endif
int utimensat(int dirfd, const char* path, const struct timespec ts[2], int flags);

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

unsigned getuid();
// Narrow CRT calls (open, rename, unlink, fopen, ...) take UTF-8 because the
// executable's manifest sets the active code page to UTF-8 (Windows 10 1903+).
#endif
