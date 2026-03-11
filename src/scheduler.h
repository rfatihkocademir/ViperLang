#ifndef VIPER_SCHEDULER_H
#define VIPER_SCHEDULER_H

#include "vm.h"

void init_scheduler(void);
void schedule_fiber(VM* fiber);
void run_scheduler_loop(void);
void stop_scheduler(void);
void destroy_scheduler(void);

#endif // VIPER_SCHEDULER_H
