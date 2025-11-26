#include "publisher.h"

#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

void *publisher_thread_func(void *arg) {
    PublisherArgs *args = (PublisherArgs *)arg;
    if (!args || !args->cfg || !args->st || !args->running) {
        return NULL;
    }

    Config cfg_local;
    StateSnapshot snap;

    while (atomic_load(args->running)) {
        config_copy(&cfg_local, args->cfg);
        state_get_snapshot(args->st, &snap);

        struct mosquitto *mosq = mqtt_get_client();
        if (mosq) {
            bool i2c_ok = snap.last_i2c_error[0] == '\0';

            if (i2c_ok) {
                mqtt_publish_sensores(&cfg_local, args->st, mosq,
                                      (int)(snap.temp_c * 100.0f),
                                      (int)(snap.umid * 100.0f),
                                      snap.duty_aplicado);
            } else {
                fprintf(stderr,
                        "Pulando publicação de sensores: erro I2C ativo (%s)\n",
                        snap.last_i2c_error);
            }

            const char *status_msg = i2c_ok ? "ok" : "error";
            const char *detail = i2c_ok ? "" : snap.last_i2c_error;
            mqtt_publish_status(&cfg_local, args->st, mosq, status_msg, detail);
        }

        double seconds = cfg_local.pub_period_s;
        if (seconds < 0) {
            seconds = 0;
        }

        struct timespec sleep_time;
        sleep_time.tv_sec = (time_t)seconds;
        sleep_time.tv_nsec = (long)((seconds - (double)sleep_time.tv_sec) * 1e9);

        nanosleep(&sleep_time, NULL);
    }

    return NULL;
}
