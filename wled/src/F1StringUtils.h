/*
 * F1StringUtils.h  –  Pure-C++ string helpers shared by F1NetWork and tests.
 * Header-only, no ESP32 / Arduino dependencies.
 */
#pragma once
#include <cstdio>
#include <cstring>
#include <cctype>

/* URL-encode src into dst (percent-encoding, RFC 3986 unreserved chars kept).
   Returns dst. */
inline char* f1_url_encode(const char* src, char* dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 4 < dst_size; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c) || c == '_' || c == '-' || c == '.' || c == '~') {
            dst[j++] = (char)c;
        } else {
            dst[j++] = '%';
            dst[j++] = hex[c >> 4];
            dst[j++] = hex[c & 0xF];
        }
    }
    dst[j] = '\0';
    return dst;
}

/* Extract a JSON string value: {"key":"value",...}  → value
   Returns true and writes to out on success.  out is always NUL-terminated. */
inline bool f1_json_str(const char* json, const char* key,
                        char* out, size_t out_size)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ' || *p == ':') ++p;
    if (*p != '"') return false;
    ++p;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < out_size) out[n++] = *p++;
    out[n] = '\0';
    return n > 0;
}
