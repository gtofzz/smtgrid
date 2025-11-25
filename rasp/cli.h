#ifndef CLI_H
#define CLI_H

#include <stdatomic.h>

#include "config.h"
#include "state.h"

typedef struct {
    Config *cfg;
    State *st;
    atomic_bool *running;
} CliArgs;

void run_cli(CliArgs *args);

#endif // CLI_H
