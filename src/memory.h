#ifndef VIPER_MEMORY_H
#define VIPER_NATIVE_H // Wait, the include in native.c is "memory.h" but it might have a guard like VIPER_MEMORY_H

#ifndef VIPER_MEMORY_H
#define VIPER_MEMORY_H

#include <stddef.h>
#include "vm.h"

void init_memory(void);
void free_memory(void);
void* viper_allocate(size_t size, int type);
void collect_garbage(VM* vm);

#endif // VIPER_MEMORY_H
#endif
