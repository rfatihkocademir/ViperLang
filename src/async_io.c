#define _GNU_SOURCE
#include "async_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/sendfile.h>

static int g_epoll_fd = -1;

static void async_error(const char* code, const char* fmt, ...) {
    va_list args;
    fprintf(stderr, "Async Error [%s]: ", code);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

void init_async_io() {
    g_epoll_fd = epoll_create1(0);
    if (g_epoll_fd == -1) {
        async_error("VAS001", "epoll_create1 failed: %s", strerror(errno));
        exit(1);
    }
}

static bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool async_register(int fd, uint32_t events, void* data) {
    if (g_epoll_fd == -1) init_async_io();
    if (!set_nonblocking(fd)) return false;

    struct epoll_event ev;
    ev.events = events;
    ev.data.ptr = data;
    return epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, fd, &ev) == 0;
}

bool async_unregister(int fd) {
    if (g_epoll_fd == -1) return false;
    return epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, fd, NULL) == 0;
}

bool async_modify(int fd, uint32_t events, void* data) {
    if (g_epoll_fd == -1) return false;
    struct epoll_event ev;
    ev.events = events;
    ev.data.ptr = data;
    return epoll_ctl(g_epoll_fd, EPOLL_CTL_MOD, fd, &ev) == 0;
}

int async_poll(int timeout_ms) {
    if (g_epoll_fd == -1) return 0;
    
    struct epoll_event events[MAX_EVENTS];
    int n = epoll_wait(g_epoll_fd, events, MAX_EVENTS, timeout_ms);
    if (n < 0) {
        if (errno == EINTR) return 0;
        async_error("VAS002", "epoll_wait failed: %s", strerror(errno));
        return -1;
    }
    
    for (int i = 0; i < n; i++) {
        // In a full implementation, we'd trigger fiber wakeups here.
        // For now, we return the count.
    }
    
    return n;
}

int async_accept(int server_fd) {
    int client_fd = accept4(server_fd, NULL, NULL, SOCK_NONBLOCK);
    if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -2; // Yield
        return -1; // Error
    }
    return client_fd;
}

int async_recv(int fd, char* buf, size_t len) {
    ssize_t n = recv(fd, buf, len, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -2; // Yield
        return -1; // Error
    }
    return (int)n;
}

int async_send(int fd, const char* buf, size_t len) {
    ssize_t n = send(fd, buf, len, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -2; // Yield
        return -1; // Error
    }
    return (int)n;
}

ssize_t async_sendfile(int out_fd, int in_fd, off_t* offset, size_t count) {
    return sendfile(out_fd, in_fd, offset, count);
}
