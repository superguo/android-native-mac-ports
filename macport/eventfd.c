// eventfd.c
#include <sys/eventfd.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>

typedef struct {
    int sock_r;           // Read end of socket pair
    int sock_w;           // Write end of socket pair
    uint64_t counter;     // Current counter value
    int flags;            // Flags (EFD_SEMAPHORE, etc.)
    pthread_mutex_t lock; // Lock for thread safety
} eventfd_ctx;

// Table to keep track of all eventfd contexts
#define MAX_EVENTFDS 1024
static eventfd_ctx *ctx_table[MAX_EVENTFDS] = {NULL};
static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;

// Find context by fd
static eventfd_ctx *find_ctx(int fd) {
    pthread_mutex_lock(&table_lock);
    for (int i = 0; i < MAX_EVENTFDS; i++) {
        if (ctx_table[i] && ctx_table[i]->sock_r == fd) {
            eventfd_ctx *ctx = ctx_table[i];
            pthread_mutex_unlock(&table_lock);
            return ctx;
        }
    }
    pthread_mutex_unlock(&table_lock);
    return NULL;
}

// Add context to table
static int add_ctx(eventfd_ctx *ctx) {
    pthread_mutex_lock(&table_lock);
    
    // Find an empty slot
    int idx = -1;
    for (int i = 0; i < MAX_EVENTFDS; i++) {
        if (ctx_table[i] == NULL) {
            idx = i;
            break;
        }
    }
    
    if (idx == -1) {
        pthread_mutex_unlock(&table_lock);
        errno = EMFILE;
        return -1;
    }
    
    ctx_table[idx] = ctx;
    pthread_mutex_unlock(&table_lock);
    return 0;
}

// Remove context from table
static void remove_ctx(int fd) {
    pthread_mutex_lock(&table_lock);
    for (int i = 0; i < MAX_EVENTFDS; i++) {
        if (ctx_table[i] && ctx_table[i]->sock_r == fd) {
            ctx_table[i] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&table_lock);
}

// Drain socket buffer
static void drain_socket(int fd) {
    char buffer[128];
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    
    while (read(fd, buffer, sizeof(buffer)) > 0) {
        // Just drain the buffer
    }
    
    fcntl(fd, F_SETFL, flags);
}

// Implementation of eventfd
int eventfd(unsigned int initval, int flags) {
    int sockets[2];
    
    // Use socketpair instead of pipe for better control
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == -1) {
        return -1;
    }
    
    // Set flags on sockets
    if (flags & EFD_NONBLOCK) {
        fcntl(sockets[0], F_SETFL, O_NONBLOCK);
        fcntl(sockets[1], F_SETFL, O_NONBLOCK);
    }
    
    if (flags & EFD_CLOEXEC) {
        fcntl(sockets[0], F_SETFD, FD_CLOEXEC);
        fcntl(sockets[1], F_SETFD, FD_CLOEXEC);
    }
    
    // Create and initialize context
    eventfd_ctx *ctx = malloc(sizeof(eventfd_ctx));
    if (!ctx) {
        close(sockets[0]);
        close(sockets[1]);
        errno = ENOMEM;
        return -1;
    }
    
    ctx->sock_r = sockets[0];
    ctx->sock_w = sockets[1];
    ctx->counter = initval;
    ctx->flags = flags;
    if (pthread_mutex_init(&ctx->lock, NULL) != 0) {
        free(ctx);
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }
    
    // Add to table
    if (add_ctx(ctx) != 0) {
        pthread_mutex_destroy(&ctx->lock);
        free(ctx);
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }
    
    // If we have an initial value, we need to signal
    if (initval > 0) {
        char buf = 1;
        write(ctx->sock_w, &buf, 1);
    }
    
    return sockets[0];  // Return the read end of the socket pair
}

// Read from eventfd
int eventfd_read(int fd, uint64_t *value) {
    eventfd_ctx *ctx = find_ctx(fd);
    if (!ctx) {
        errno = EBADF;
        return -1;
    }
    
    pthread_mutex_lock(&ctx->lock);
    
    if (ctx->counter == 0) {
        if (ctx->flags & EFD_NONBLOCK) {
            pthread_mutex_unlock(&ctx->lock);
            errno = EAGAIN;
            return -1;
        }
        
        // Block until data is available
        pthread_mutex_unlock(&ctx->lock);
        char dummy;
        ssize_t result = read(ctx->sock_r, &dummy, 1);
        if (result < 0) {
            return -1;
        }
        pthread_mutex_lock(&ctx->lock);
    } else {
        // Drain any pending signals from the socket
        drain_socket(ctx->sock_r);
    }
    
    // Handle read based on semaphore flag
    if (ctx->flags & EFD_SEMAPHORE) {
        *value = 1;
        ctx->counter--;
    } else {
        *value = ctx->counter;
        ctx->counter = 0;
    }
    
    pthread_mutex_unlock(&ctx->lock);
    return 0;
}

// Write to eventfd
int eventfd_write(int fd, uint64_t value) {
    eventfd_ctx *ctx = find_ctx(fd);
    if (!ctx) {
        errno = EBADF;
        return -1;
    }
    
    if (value == UINT64_MAX) {
        errno = EINVAL;
        return -1;
    }
    
    pthread_mutex_lock(&ctx->lock);
    
    uint64_t old_counter = ctx->counter;
    if (UINT64_MAX - old_counter < value) {
        pthread_mutex_unlock(&ctx->lock);
        errno = EAGAIN;
        return -1;
    }
    
    // Update counter
    ctx->counter += value;
    
    // Signal any waiting readers
    char dummy = 1;
    ssize_t result = write(ctx->sock_w, &dummy, 1);
    
    pthread_mutex_unlock(&ctx->lock);
    
    if (result < 0) {
        return -1;
    }
    
    return 0;
}

// Close an eventfd
int eventfd_close(int fd) {
    eventfd_ctx *ctx = find_ctx(fd);
    if (!ctx) {
        errno = EBADF;
        return -1;
    }
    
    // Clean up
    close(ctx->sock_r);
    close(ctx->sock_w);
    pthread_mutex_destroy(&ctx->lock);
    remove_ctx(fd);
    free(ctx);
    
    return 0;
}