/*
 * platform.h - Cross-platform helpers
 */
#ifndef MUSE_PLATFORM_H
#define MUSE_PLATFORM_H

#include <stdio.h>

#ifdef _WIN32
#include <windows.h>

/* fopen wrapper that handles UTF-8 paths on Windows */
static inline FILE *fopen_utf8(const char *path, const char *mode)
{
    /* convert UTF-8 path to UTF-16 */
    int wpath_len = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    int wmode_len = MultiByteToWideChar(CP_UTF8, 0, mode, -1, NULL, 0);
    if (wpath_len <= 0 || wmode_len <= 0) return fopen(path, mode);

    wchar_t wpath[4096], wmode[16];
    if (wpath_len > 4096 || wmode_len > 16) return fopen(path, mode);

    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wpath_len);
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, wmode_len);
    return _wfopen(wpath, wmode);
}

#else
/* On Linux/macOS, fopen already handles UTF-8 natively */
static inline FILE *fopen_utf8(const char *path, const char *mode)
{
    return fopen(path, mode);
}
#endif

#endif /* MUSE_PLATFORM_H */
