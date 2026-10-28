#pragma once
#include <stddef.h>
extern "C" { void* memset(void*,int,size_t); void* memcpy(void*,const void*,size_t); int memcmp(const void*,const void*,size_t); size_t strlen(const char*); char* strncpy(char*,const char*,size_t); int strcmp(const char*,const char*); int strncmp(const char*,const char*,size_t); }
