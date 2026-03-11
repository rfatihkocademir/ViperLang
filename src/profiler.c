#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include "profiler.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

ViperProfiler g_profiler;

static uint64_t get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

void profiler_init(void) {
    memset(&g_profiler, 0, sizeof(ViperProfiler));
    g_profiler.vm_start_time_us = get_time_us();
}

void profiler_track_alloc(size_t size, int obj_type) {
    if (obj_type >= 0 && obj_type < PROF_TYPE_COUNT) {
        g_profiler.alloc_count[obj_type]++;
    }
    g_profiler.alloc_bytes += (uint64_t)size;
    
    uint64_t live = profiler_live_memory();
    if (live > g_profiler.peak_memory) {
        g_profiler.peak_memory = live;
    }
}

void profiler_track_free(size_t size, int obj_type) {
    if (obj_type >= 0 && obj_type < PROF_TYPE_COUNT) {
        g_profiler.free_count[obj_type]++;
    }
    g_profiler.freed_bytes += (uint64_t)size;
}

void profiler_track_gc(int is_major, uint64_t pause_us) {
    if (is_major) {
        g_profiler.gc_major_count++;
    } else {
        g_profiler.gc_minor_count++;
    }
    g_profiler.gc_total_pause_us += pause_us;
}

void profiler_track_jit(void) {
    g_profiler.jit_compilations++;
}

void profiler_track_instructions(uint64_t count) {
    g_profiler.total_instructions += count;
}

uint64_t profiler_live_memory(void) {
    if (g_profiler.alloc_bytes > g_profiler.freed_bytes) {
        return g_profiler.alloc_bytes - g_profiler.freed_bytes;
    }
    return 0;
}

static const char* type_name(int type) {
    switch(type) {
        case 0: return "String";
        case 1: return "Array";
        case 2: return "Table";
        case 3: return "Function";
        case 4: return "Fiber";
        case 5: return "Module";
        case 6: return "Box";
        case 7: return "Native";
        case 8: return "UserObj";
        default: return "Other";
    }
}

void profiler_print_snapshot(void) {
    uint64_t now = get_time_us();
    double uptime = (double)(now - g_profiler.vm_start_time_us) / 1000000.0;
    uint64_t live = profiler_live_memory();
    
    printf("\n╔══════════════════════════════════════════════════════════╗\n");
    printf("║           ViperLang Memory Profiler Snapshot            ║\n");
    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║ VM Uptime:             %8.3f sec                       ║\n", uptime);
    printf("║ Instructions:          %10llu ops                      ║\n", (unsigned long long)g_profiler.total_instructions);
    
    if (uptime > 0.001) {
        printf("║ Throughput:            %10.0f ops/sec                  ║\n", (double)g_profiler.total_instructions / uptime);
    }
    
    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║ MEMORY                                                  ║\n");
    printf("║   Total Allocated:     %10llu bytes                     ║\n", (unsigned long long)g_profiler.alloc_bytes);
    printf("║   Total Freed:         %10llu bytes                     ║\n", (unsigned long long)g_profiler.freed_bytes);
    printf("║   Live Memory:         %10llu bytes                     ║\n", (unsigned long long)live);
    printf("║   Peak Memory:         %10llu bytes                     ║\n", (unsigned long long)g_profiler.peak_memory);
    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║ OBJECT ALLOCATIONS          Alloc    Freed    Live      ║\n");
    
    for (int i = 0; i < PROF_TYPE_COUNT; i++) {
        if (g_profiler.alloc_count[i] > 0) {
            uint64_t l = g_profiler.alloc_count[i] - g_profiler.free_count[i];
            printf("║   %-18s  %7llu  %7llu  %7llu      ║\n", 
                   type_name(i),
                   (unsigned long long)g_profiler.alloc_count[i],
                   (unsigned long long)g_profiler.free_count[i],
                   (unsigned long long)l);
        }
    }
    
    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║ GC                                                      ║\n");
    printf("║   Minor Collections:       %7llu                      ║\n", (unsigned long long)g_profiler.gc_minor_count);
    printf("║   Major Collections:       %7llu                      ║\n", (unsigned long long)g_profiler.gc_major_count);
    printf("║   Total GC Pause:      %10.3f ms                         ║\n", (double)g_profiler.gc_total_pause_us / 1000.0);
    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║ JIT Compilations:          %7llu                      ║\n", (unsigned long long)g_profiler.jit_compilations);
    printf("╚══════════════════════════════════════════════════════════╝\n\n");
}
