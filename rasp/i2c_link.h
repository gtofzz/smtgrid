#ifndef I2C_LINK_H
#define I2C_LINK_H

#include <stdatomic.h>

#include "config.h"
#include "state.h"

typedef struct {
    Config *cfg;
    State *st;
    atomic_bool *running;
} I2CThreadArgs;

void *i2c_thread_func(void *arg);

#endif // I2C_LINK_H
