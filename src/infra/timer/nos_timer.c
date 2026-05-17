#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nos_timer_api.h"
#include "nos_api.h"

static nos_timer_ops_t g_timer_ops = {
    .start = nos_timer_start,
    .stop = nos_timer_stop
};

void nos_timer_init(void) {
    static int initialized = 0;
    if (initialized) return;

    extern nos_status_t nos_embedded_service_register(const char *name, void *ops);
    nos_embedded_service_register("SVC_TIMER", &g_timer_ops);
    
    initialized = 1;
}
