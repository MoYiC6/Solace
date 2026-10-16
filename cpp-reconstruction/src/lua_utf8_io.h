#ifndef SOLACE_LUA_UTF8_IO_H
#define SOLACE_LUA_UTF8_IO_H

/* Include CRT declarations before redirecting their names in official Lua C. */
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif
FILE* solace_lua_fopen_utf8(const char* filename, const char* mode);
FILE* solace_lua_freopen_utf8(const char* filename, const char* mode, FILE* stream);
#ifdef __cplusplus
}
#endif

/* Set only for lauxlib.c, loadlib.c and liolib.c; never for this bridge's C file. */
#ifdef SOLACE_LUA_UTF8_IO_REDIRECT
#define fopen solace_lua_fopen_utf8
#define freopen solace_lua_freopen_utf8
#endif
#endif
