// eventfd.h
#ifndef EVENTFD_H
#define EVENTFD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Define flags to match Linux eventfd
#define EFD_CLOEXEC 1
#define EFD_NONBLOCK 2
#define EFD_SEMAPHORE 4

// Main API functions
int eventfd(unsigned int initval, int flags);
int eventfd_read(int fd, uint64_t *value);
int eventfd_write(int fd, uint64_t value);
int eventfd_close(int fd);

// Additional helper types/functions for full Linux compatibility
typedef uint64_t eventfd_t;

static inline int eventfd_signal(int fd, eventfd_t value) {
    return eventfd_write(fd, value);
}

static inline int eventfd_clear(int fd, eventfd_t *value) {
    return eventfd_read(fd, value);
}

#ifdef __cplusplus
} // extern "C"
#endif

#endif // EVENTFD_H
