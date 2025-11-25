#include "state.h"

#include <stdio.h>
#include <string.h>

void state_init(State *st) {
    if (!st) {
        return;
    }
    memset(st, 0, sizeof(*st));
    pthread_mutex_init(&st->lock, NULL);
}

void state_destroy(State *st) {
    if (!st) {
        return;
    }
    pthread_mutex_destroy(&st->lock);
}

void state_set_duty_req(State *st, int duty) {
    if (!st) {
        return;
    }
    if (duty < 0) {
        duty = 0;
    }
    if (duty > 100) {
        duty = 100;
    }
    pthread_mutex_lock(&st->lock);
    st->duty_req = duty;
    pthread_mutex_unlock(&st->lock);
}

void state_set_feedback(State *st, int duty_aplicado, float temp_c, float umid) {
    if (!st) {
        return;
    }
    pthread_mutex_lock(&st->lock);
    st->duty_aplicado = duty_aplicado;
    st->temp_c = temp_c;
    st->umid = umid;
    pthread_mutex_unlock(&st->lock);
}

void state_set_i2c_error(State *st, const char *msg) {
    if (!st) {
        return;
    }
    pthread_mutex_lock(&st->lock);
    if (msg) {
        snprintf(st->last_i2c_error, sizeof(st->last_i2c_error), "%s", msg);
    } else {
        st->last_i2c_error[0] = '\0';
    }
    pthread_mutex_unlock(&st->lock);
}

void state_clear_i2c_error(State *st) {
    state_set_i2c_error(st, "");
}

void state_set_mqtt_error(State *st, const char *msg) {
    if (!st) {
        return;
    }
    pthread_mutex_lock(&st->lock);
    if (msg) {
        snprintf(st->last_mqtt_error, sizeof(st->last_mqtt_error), "%s", msg);
    } else {
        st->last_mqtt_error[0] = '\0';
    }
    pthread_mutex_unlock(&st->lock);
}

void state_clear_mqtt_error(State *st) {
    state_set_mqtt_error(st, "");
}

void state_get_snapshot(State *st, StateSnapshot *out) {
    if (!st || !out) {
        return;
    }
    pthread_mutex_lock(&st->lock);
    out->duty_req = st->duty_req;
    out->duty_aplicado = st->duty_aplicado;
    out->temp_c = st->temp_c;
    out->umid = st->umid;
    snprintf(out->last_i2c_error, sizeof(out->last_i2c_error), "%s",
             st->last_i2c_error);
    snprintf(out->last_mqtt_error, sizeof(out->last_mqtt_error), "%s",
             st->last_mqtt_error);
    pthread_mutex_unlock(&st->lock);
}
