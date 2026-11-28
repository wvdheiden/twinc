// Shadows MinGW's <sys/stat.h> (src/win is first on the include path).
#pragma once
#include_next <sys/stat.h>
#include "../compat.h"
