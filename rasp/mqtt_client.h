#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <mosquitto.h>
#include <pthread.h>
#include <stdatomic.h>

#include "config.h"
#include "state.h"

typedef struct {
    Config *cfg;
    State *st;
    atomic_bool *running;
} MqttClientArgs;

int mqtt_client_start(const MqttClientArgs *args);
void mqtt_client_stop(void);

int mqtt_publish_sensores(const Config *cfg, State *st, struct mosquitto *mosq,
                          int temp_c, int umid, int pwm);
int mqtt_publish_status(const Config *cfg, State *st, struct mosquitto *mosq,
                        const char *status, const char *msg);

struct mosquitto *mqtt_get_client(void);

#endif // MQTT_CLIENT_H
