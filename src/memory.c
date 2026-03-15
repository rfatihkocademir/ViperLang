#define _POSIX_C_SOURCE 199309L
#include "memory.h"
#include "profiler.h"
#include "native.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#define NURSERY_SIZE (8 * 1024 * 1024) // 8MB Nursery

static uint8_t* nursery_start = NULL;
static uint8_t* nursery_top = NULL;
static uint8_t* nursery_end = NULL;

static void memory_error_exit(const char* code, const char* fmt, ...) {
    va_list args;
    fprintf(stderr, "Memory Error [%s]: ", code);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    exit(1);
}

static uint64_t get_time_us_local(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

void init_memory(void) {
    nursery_start = malloc(NURSERY_SIZE);
    if (!nursery_start) {
        memory_error_exit("VME001", "Could not allocate nursery.");
    }
    nursery_top = nursery_start;
    nursery_end = nursery_start + NURSERY_SIZE;
    
    profiler_init();
}

void free_memory(void) {
    if (nursery_start) free(nursery_start);
}

void* viper_allocate(size_t size, int type) {
    // 1. Try to allocate in Nursery (Bump-pointer)
    size_t aligned_size = (size + 7) & ~7;
    if (nursery_top + aligned_size <= nursery_end) {
        void* ptr = nursery_top;
        nursery_top += aligned_size;
        
        // Basic object header initialization
        Obj* obj = (Obj*)ptr;
        obj->type = type;
        obj->ref_count = 0;
        
        profiler_track_alloc(size, type);
        return ptr;
    }

    // 2. Nursery Full -> Trigger Minor GC (Stub for now: just fallback to malloc)
    // collect_garbage_minor();
    
    void* ptr = malloc(size);
    if (!ptr) {
        memory_error_exit("VME002", "Out of memory.");
    }
    
    // Header for heap objects
    Obj* obj = (Obj*)ptr;
    obj->type = type;
    obj->ref_count = 0;
    
    profiler_track_alloc(size, type);
    return ptr;
}

void collect_garbage(VM* vm) {
    (void)vm;
    uint64_t start = get_time_us_local();
    
    // Minor GC: Reset nursery bump pointer
    // In a full implementation, we'd copy live objects from nursery to Gen1 first.
    nursery_top = nursery_start;
    
    uint64_t end = get_time_us_local();
    profiler_track_gc(1, end - start);
}
