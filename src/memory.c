#define _POSIX_C_SOURCE 199309L
#include "memory.h"
#include "profiler.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static uint64_t get_time_us_local(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

void init_memory(void) {
    // Initialize memory pools or GC state here
    profiler_init();
}

void free_memory(void) {
    // Sweep everything remaining
}

void* viper_allocate(size_t size, int type) {
    void* ptr = malloc(size);
    if (!ptr) {
        fprintf(stderr, "Fatal: Out of memory\n");
        exit(1);
    }
    profiler_track_alloc(size, type);
    return ptr;
}

void collect_garbage(VM* vm) {
    (void)vm; // Placeholder for now
    uint64_t start = get_time_us_local();
    
    // In a real implementation, we'd mark and sweep here.
    // For now, we just track the GC event for the profiler.
    
    uint64_t end = get_time_us_local();
    profiler_track_gc(1, end - start);
}
