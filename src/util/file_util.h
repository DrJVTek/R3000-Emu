#pragma once

#include <cstdio>

#if defined(_WIN32)
// For UE5 compatibility: use minimal Windows headers
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Forward declare just what we need to avoid pulling in windows.h
extern "C" {
    __declspec(dllimport) int __stdcall MultiByteToWideChar(
        unsigned int CodePage, unsigned long dwFlags,
        const char* lpMultiByteStr, int cbMultiByte,
        wchar_t* lpWideCharStr, int cchWideChar);
}
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif
#include <malloc.h>  // for _alloca
#endif

namespace util
{

/// Open a file with UTF-8 path support on Windows.
/// On non-Windows platforms, falls back to std::fopen.
inline std::FILE* fopen_utf8(const char* path, const char* mode)
{
    if (!path || !mode)
        return nullptr;

#if defined(_WIN32)
    // Use Windows API for proper UTF-8 to UTF-16 conversion
    int wpath_len = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    int wmode_len = MultiByteToWideChar(CP_UTF8, 0, mode, -1, nullptr, 0);
    if (wpath_len <= 0 || wmode_len <= 0)
        return nullptr;

    wchar_t* wpath = static_cast<wchar_t*>(_alloca(wpath_len * sizeof(wchar_t)));
    wchar_t* wmode = static_cast<wchar_t*>(_alloca(wmode_len * sizeof(wchar_t)));

    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wpath_len);
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, wmode_len);

    return _wfopen(wpath, wmode);
#else
    return std::fopen(path, mode);
#endif
}

} // namespace util
