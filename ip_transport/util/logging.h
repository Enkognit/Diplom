#ifndef LOGGING_H
#define LOGGING_H

#include <cstring>
#include <string>

#define LEVEL_TRACE 1
#define LEVEL_DEBUG 2
#define LEVEL_INFO 3
#define LEVEL_WARN 4
#define LEVEL_ERROR 5
#define LEVEL_FATAL 6

/*
 * Set LOG_LEVEL to the minimum level of logging required
 */
#ifndef LOG_LEVEL
#ifdef BUILD_TYPE_DEBUG
#define LOG_LEVEL LEVEL_DEBUG
#else
#define LOG_LEVEL LEVEL_INFO
#endif
#endif // LOG_LEVEL

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/time.h>

#include "securec.h"

#ifndef TAG
#define TAG "LOG"
#endif

[[maybe_unused]] static inline void get_datetime_str(char *buffer, size_t len, bool short_variant)
{
    /* get current time with usec resolution */
    struct timeval tval {};
    gettimeofday(&tval, nullptr);

    /* convert current time to calendar date */
    struct tm tm_info {};
    time_t timeval = tval.tv_sec;
    localtime_r(&timeval, &tm_info);

    if (short_variant) {
        snprintf(buffer, len, "%.2d:%.2d:%.2d.%.6ld", tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec, tval.tv_usec);
    } else {
        snprintf(buffer, len, "%.4d-%.2d-%.2d %.2d:%.2d:%.2d.%.6ld", tm_info.tm_year + 1900, tm_info.tm_mon + 1,
                 tm_info.tm_mday, tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec, tval.tv_usec);
    }
}

static void StripFormatString(const std::string &prefix, std::string &str)
{
    for (auto pos = str.find(prefix, 0); pos != std::string::npos; pos = str.find(prefix, pos)) {
        str.erase(pos, prefix.size());
    }
}

[[maybe_unused]] static void PrintLog(const char *fmt, ...)
{
    std::string new_fmt(fmt);
    StripFormatString("{public}", new_fmt);
    StripFormatString("{private}", new_fmt);

    va_list args;
    va_start(args, fmt);

    vprintf(new_fmt.c_str(), args);
    (void)fputc('\n', stdout);

    va_end(args);

    fflush(stdout);
}

#define LOG(l, fmt, ...)                                                                          \
    do {                                                                                          \
        char time_buf[100] = {0};                                                                 \
        get_datetime_str(time_buf, 100, true);                                                    \
        PrintLog("[%s]%s:%s:%s:" fmt, time_buf, TAG, l, __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__); \
    } while (0)

#if LOG_LEVEL > LEVEL_TRACE
#define LOGT(...)
#else
#define LOGT(fmt, ...) LOG("TRACE", fmt __VA_OPT__(, ) __VA_ARGS__)
#endif

#if LOG_LEVEL > LEVEL_DEBUG
#define LOGD(...)
#else
#define LOGD(fmt, ...) LOG("DEBUG", fmt __VA_OPT__(, ) __VA_ARGS__)
#endif

#if LOG_LEVEL > LEVEL_INFO
#define LOGI(...)
#else
#define LOGI(fmt, ...) LOG("INFO", fmt __VA_OPT__(, ) __VA_ARGS__)
#endif

#if LOG_LEVEL > LEVEL_WARN
#define LOGW(...)
#else
#define LOGW(fmt, ...) LOG("WARN", fmt __VA_OPT__(, ) __VA_ARGS__)
#endif

#if LOG_LEVEL > LEVEL_ERROR
#define LOGE(...)
#else
#define LOGE(fmt, ...) LOG("ERROR", fmt __VA_OPT__(, ) __VA_ARGS__)
#endif

#define LOGF(fmt, ...) LOG("FATAL", fmt __VA_OPT__(, ) __VA_ARGS__)

#endif

