#include "publisher.h"

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
            mqtt_publish_sensores(&cfg_local, args->st, mosq,
                                  (int)(snap.temp_c * 100.0f),
                                  (int)(snap.umid * 100.0f), snap.duty_aplicado);

            const char *status_msg = snap.last_i2c_error[0] ? "error" : "ok";
            const char *detail = snap.last_i2c_error[0] ? snap.last_i2c_error
                                                         : "";
            mqtt_publish_status(&cfg_local, args->st, mosq, status_msg, detail);
        }

        usleep((useconds_t)(cfg_local.pub_period_s * 1e6));
    }

    return NULL;
}
