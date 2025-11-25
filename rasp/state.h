#ifndef STATE_H
#define STATE_H

#include <pthread.h>

// Estrutura de estado compartilhado entre as threads.
typedef struct {
    int duty_req;
    int duty_aplicado;
    float temp_c;
    float umid;
    char last_i2c_error[256];
    char last_mqtt_error[256];
    pthread_mutex_t lock;
} State;

// Cópia segura do estado para leitura atômica.
typedef struct {
    int duty_req;
    int duty_aplicado;
    float temp_c;
    float umid;
    char last_i2c_error[256];
    char last_mqtt_error[256];
} StateSnapshot;

void state_init(State *st);
void state_destroy(State *st);

void state_set_duty_req(State *st, int duty);
void state_set_feedback(State *st, int duty_aplicado, float temp_c, float umid);
void state_set_i2c_error(State *st, const char *msg);
void state_clear_i2c_error(State *st);
void state_set_mqtt_error(State *st, const char *msg);
void state_clear_mqtt_error(State *st);
void state_get_snapshot(State *st, StateSnapshot *out);

#endif // STATE_H
