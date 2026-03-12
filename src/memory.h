#ifndef VIPER_MEMORY_H
#define VIPER_MEMORY_H

#include <stddef.h>
#include "vm.h"

void init_memory(void);
void free_memory(void);
void* viper_allocate(size_t size, int type);
void collect_garbage(VM* vm);

#endif // VIPER_MEMORY_H
