#include "async_io.h"
#include <sys/epoll.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>

static int epoll_fd = -1;

void init_async_io(void) {
    if (epoll_fd != -1) close(epoll_fd);
    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll_create1");
    }
}

bool async_io_register(int fd, int events) {
    if (epoll_fd == -1) return false;
    
    struct epoll_event ev;
    ev.events = events | EPOLLET; // Edge-triggered
    ev.data.fd = fd;
    
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        if (errno == EEXIST) {
            return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) != -1;
        }
        return false;
    }
    return true;
}

void async_io_deregister(int fd) {
    if (epoll_fd != -1) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    }
}

void async_io_poll(void) {
    if (epoll_fd == -1) return;
    
    struct epoll_event events[64];
    int nfds = epoll_wait(epoll_fd, events, 64, 0); // No wait
    
    for (int i = 0; i < nfds; i++) {
        // In a full implementation, we'd notify the scheduler or wake the fiber
        // for now we just acknowledge the event.
    }
}
