#ifndef VIPER_PROFILER_H
#define VIPER_PROFILER_H

#include <stdint.h>
#include <stddef.h>

#define PROF_TYPE_COUNT 20 // Adjust based on vm.h

typedef struct {
    uint64_t alloc_count[PROF_TYPE_COUNT];
    uint64_t free_count[PROF_TYPE_COUNT];
    uint64_t alloc_bytes;
    uint64_t freed_bytes;
    uint64_t peak_memory;
    
    // GC Stats
    uint64_t gc_minor_count;
    uint64_t gc_major_count;
    uint64_t gc_total_pause_us;
    
    // JIT Stats
    uint64_t jit_compilations;
    
    // VM Stats
    uint64_t total_instructions;
    uint64_t vm_start_time_us;
} ViperProfiler;

extern ViperProfiler g_profiler;

void profiler_init(void);
void profiler_track_alloc(size_t size, int obj_type);
void profiler_track_free(size_t size, int obj_type);
void profiler_track_gc(int is_major, uint64_t pause_us);
void profiler_track_jit(void);
void profiler_track_instructions(uint64_t count);
uint64_t profiler_live_memory(void);
void profiler_print_snapshot(void);

#endif // VIPER_PROFILER_H
