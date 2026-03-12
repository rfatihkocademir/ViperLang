#ifndef VIPER_ASYNC_IO_H
#define VIPER_ASYNC_IO_H

#include "vm.h"
#include <sys/epoll.h>

// Max events per epoll_wait call
#define MAX_EVENTS 64

// Asynchronous I/O Event Loop state
typedef struct {
    int epoll_fd;
    bool running;
} AsyncLoop;

// Initialize the global async event loop
void init_async_io();

// Add a file descriptor to the event loop
// mode: EPOLLIN | EPOLLOUT | EPOLLET
bool async_register(int fd, uint32_t events, void* data);

// Remove a file descriptor from the event loop
bool async_unregister(int fd);

// Modify an existing registration
bool async_modify(int fd, uint32_t events, void* data);

// Step the event loop (non-blocking or with timeout)
// timeout_ms: -1 for infinite, 0 for non-blocking
int async_poll(int timeout_ms);

// Non-blocking socket primitives
int async_accept(int server_fd);
int async_recv(int fd, char* buf, size_t len);
int async_send(int fd, const char* buf, size_t len);

// Zero-copy file serving
ssize_t async_sendfile(int out_fd, int in_fd, off_t* offset, size_t count);

#endif // VIPER_ASYNC_IO_H
