#pragma once
#include <stddef.h>
typedef struct FILE FILE;
extern "C" { int printf(const char*,...); int snprintf(char*,size_t,const char*,...); int _snprintf(char*,size_t,const char*,...); }
