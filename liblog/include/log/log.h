#ifndef LOG_LOG_H
#define LOG_LOG_H

#include <cstdio>
#include <cstdlib>

extern "C" {

#define LOG_FATAL_IF(condition, format, ...) \
    if (condition) { \
        fprintf(stderr, "Fatal error: " format "\n", ##__VA_ARGS__); \
        abort(); \
    }


#define LOG_ALWAYS_FATAL(format, ...) \
    fprintf(stderr, "Fatal error: " format "\n", ##__VA_ARGS__); \
    abort()

#define __android_second(dummy, second, ...) second
#define __android_rest(first, ...) , ##__VA_ARGS__


#define LOG_ALWAYS_FATAL_IF(condition, ...) \
    if (condition) { \
        fprintf(stderr, "Fatal error: " __android_second(0, ##__VA_ARGS__, "") "\n" __android_rest(__VA_ARGS__)); \
        abort(); \
    }

#ifdef _DEBUG
#define ALOGW(format, ...) fprintf(stderr, "Warning: " format "\n", ##__VA_ARGS__)
#define ALOGD(format, ...) fprintf(stderr, "Debug: " format "\n", ##__VA_ARGS__)
#define ALOGE(format, ...) fprintf(stderr, "Error: " format "\n", ##__VA_ARGS__)
#define ALOGW_IF(condition, format, ...) \
    if (condition) { \
        fprintf(stderr, "Warning: " format "\n", ##__VA_ARGS__); \
    }
#define ALOG_ASSERT(cond, ...) LOG_FATAL_IF(!(cond), ##__VA_ARGS__)
#else
#define ALOGW(...) (void)0
#define ALOGD(...) (void)0
#define ALOGE(...) (void)0
#define ALOGW_IF(...) (void)0
#define ALOG_ASSERT(...) (void)0
#endif
}
#endif
