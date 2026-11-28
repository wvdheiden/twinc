// Minimal getpwuid: the current user's name and profile directory.
#pragma once
struct passwd {
  const char* pw_name;
  const char* pw_dir;
};
struct passwd* getpwuid(unsigned uid);
