#ifndef PUBLISHER_H
#define PUBLISHER_H

#include <stdatomic.h>

#include "config.h"
#include "state.h"

#include "mqtt_client.h"

typedef struct {
    Config *cfg;
    State *st;
    atomic_bool *running;
} PublisherArgs;

void *publisher_thread_func(void *arg);

#endif // PUBLISHER_H
