#define _DEFAULT_SOURCE
#include "scheduler.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <unistd.h>

#define MAX_FIBERS 1024

static VM* fiber_queue[MAX_FIBERS];
static atomic_int queue_head = 0;
static atomic_int queue_tail = 0;
static atomic_bool running = true;

void init_scheduler(void) {
    // For simplicity in this restoration, we'll use the main thread as the worker
}

void destroy_scheduler(void) {
    stop_scheduler();
}

void schedule_fiber(VM* fiber) {
    int tail = atomic_load(&queue_tail);
    int next_tail = (tail + 1) % MAX_FIBERS;
    
    // In a production version, we'd wait or grow if full
    if (next_tail == atomic_load(&queue_head)) return; 
    
    fiber_queue[tail] = fiber;
    atomic_store(&queue_tail, next_tail);
}

static VM* pop_fiber(void) {
    int head = atomic_load(&queue_head);
    if (head == atomic_load(&queue_tail)) return NULL;
    
    VM* fiber = fiber_queue[head];
    atomic_store(&queue_head, (head + 1) % MAX_FIBERS);
    return fiber;
}

void run_scheduler_loop(void) {
    while (atomic_load(&running)) {
        VM* fiber = pop_fiber();
        if (!fiber) {
            int empty_waits = 0;
            while (empty_waits < 100) {
                fiber = pop_fiber();
                if (fiber) break;
                usleep(100); 
                empty_waits++;
                if (!atomic_load(&running)) break;
            }
            if (!fiber) break; // Exhausted
        }
        
        InterpretResult res = interpret(fiber, NULL, 500); // 500 steps per slice
        
        if (res == INTERPRET_YIELD) {
            schedule_fiber(fiber);
        } else if (res == INTERPRET_OK) {
            // Fiber finished, cleanup VM if it was a dynamically allocated one
            // In ViperLang v3, the main vm is managed by run_file
        } else if (res == INTERPRET_ERROR) {
            // Fiber crashed
        }
    }
}

void stop_scheduler(void) {
    atomic_store(&running, false);
}
