#pragma once
#define FNM_NOMATCH 1
#define FNM_CASEFOLD 0x10
int fnmatch(const char* pattern, const char* name, int flags);
