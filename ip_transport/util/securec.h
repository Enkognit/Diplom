#pragma once

#include <cassert>
#include <cstring>

#define EOK 0

#define sprintf_s(s, s_size, ...) sprintf(s __VA_OPT__(, ) __VA_ARGS__)

#define vsnprintf_s(s, size, s_size, ...) vsnprintf(s, size __VA_OPT__(, ) __VA_ARGS__)

static inline int memset_s(void *data, size_t capacity, int ch, size_t count)
{
    assert(count <= capacity);
    memset(data, ch, count);
    return 0;
}

static inline int memcpy_s(void *dst, size_t capacity, const void *src, size_t count)
{
    assert(count <= capacity);
    memcpy(dst, src, count);
    return 0;
}
