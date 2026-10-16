#include "lua_utf8_io.h"
#include <Windows.h>
#include <errno.h>
#include <stdlib.h>

/* The official Lua loader/searcher stays in charge of file parsing and modes.
   Only the Windows filename/mode encoding boundary is adapted here. */
static void conversion_error(DWORD error) {
    errno = error == ERROR_NO_UNICODE_TRANSLATION ? EILSEQ : EINVAL;
    _set_doserrno(error);
}

static wchar_t* wide_utf8(const char* text) {
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    wchar_t* wide;
    if (count == 0) {
        conversion_error(GetLastError());
        return NULL;
    }
    wide = (wchar_t*)malloc((size_t)count * sizeof(wchar_t));
    if (wide == NULL) {
        errno = ENOMEM;
        _set_doserrno(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, count) == 0) {
        const DWORD error = GetLastError();
        free(wide);
        conversion_error(error);
        return NULL;
    }
    return wide;
}

static void release_preserving_error(wchar_t* filename, wchar_t* mode) {
    const int saved_errno = errno;
    unsigned long saved_doserrno = 0;
    _get_doserrno(&saved_doserrno);
    free(filename);
    free(mode);
    errno = saved_errno;
    _set_doserrno(saved_doserrno);
}

FILE* solace_lua_fopen_utf8(const char* filename, const char* mode) {
    wchar_t* wide_filename;
    wchar_t* wide_mode;
    FILE* result;
    if (filename == NULL || mode == NULL || filename[0] == '\0' || mode[0] == '\0') {
        errno = EINVAL;
        return NULL;
    }
    wide_filename = wide_utf8(filename);
    if (wide_filename == NULL) return NULL;
    wide_mode = wide_utf8(mode);
    if (wide_mode == NULL) {
        release_preserving_error(wide_filename, NULL);
        return NULL;
    }
    result = _wfopen(wide_filename, wide_mode);
    release_preserving_error(wide_filename, wide_mode);
    return result;
}

FILE* solace_lua_freopen_utf8(const char* filename, const char* mode, FILE* stream) {
    wchar_t* wide_filename;
    wchar_t* wide_mode;
    FILE* result;
    if (filename == NULL) {
        /* Windows UCRT does not support freopen(NULL, ...). */
        errno = EBADF;
        return NULL;
    }
    if (mode == NULL || stream == NULL || filename[0] == '\0') {
        errno = EINVAL;
        return NULL;
    }
    wide_filename = wide_utf8(filename);
    wide_mode = wide_filename != NULL ? wide_utf8(mode) : NULL;
    if (wide_filename == NULL || wide_mode == NULL) {
        const int saved_errno = errno;
        unsigned long saved_doserrno = 0;
        _get_doserrno(&saved_doserrno);
        /* luaL_loadfilex discards its old FILE* after a failed freopen.
           A conversion/allocation failure must close it just as a failed
           native re-open does. fclose may change errno; retain the cause. */
        fclose(stream);
        free(wide_filename);
        free(wide_mode);
        errno = saved_errno;
        _set_doserrno(saved_doserrno);
        return NULL;
    }
    result = _wfreopen(wide_filename, wide_mode, stream);
    release_preserving_error(wide_filename, wide_mode);
    return result;
}
