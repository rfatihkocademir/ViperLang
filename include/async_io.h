#ifndef VIPER_ASYNC_IO_H
#define VIPER_ASYNC_IO_H

#include <stdbool.h>

void init_async_io(void);
void async_io_poll(void);
bool async_io_register(int fd, int events);
void async_io_deregister(int fd);

#endif // VIPER_ASYNC_IO_H
