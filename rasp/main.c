#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "config.h"
#include "i2c_link.h"
#include "mqtt_client.h"
#include "publisher.h"
#include "state.h"

static atomic_bool running = true;

static void handle_sigint(int sig) {
    (void)sig;
    atomic_store(&running, false);
}

int main(void) {
    signal(SIGINT, handle_sigint);

    Config cfg;
    config_init_defaults(&cfg);
    if (config_load_mqtt_from_file(&cfg, "mqtt.conf")) {
        printf("MQTT config carregada de mqtt.conf\n");
    } else {
        printf("mqtt.conf não encontrado ou inválido, usando defaults.\n");
    }
    config_print(&cfg);

    State st;
    state_init(&st);

    bool i2c_started = false;
    bool pub_started = false;

    MqttClientArgs mqtt_args = {.cfg = &cfg, .st = &st, .running = &running};
    if (mqtt_client_start(&mqtt_args) != 0) {
        fprintf(stderr, "Falha ao iniciar MQTT, encerrando.\n");
        return EXIT_FAILURE;
    }

    I2CThreadArgs i2c_args = {.cfg = &cfg, .st = &st, .running = &running};
    pthread_t i2c_thread;
    if (pthread_create(&i2c_thread, NULL, i2c_thread_func, &i2c_args) != 0) {
        fprintf(stderr, "Não foi possível criar thread I2C.\n");
        atomic_store(&running, false);
    } else {
        i2c_started = true;
    }

    PublisherArgs pub_args = {.cfg = &cfg, .st = &st, .running = &running};
    pthread_t pub_thread;
    if (pthread_create(&pub_thread, NULL, publisher_thread_func, &pub_args) != 0) {
        fprintf(stderr, "Não foi possível criar thread Publisher.\n");
        atomic_store(&running, false);
    } else {
        pub_started = true;
    }

    CliArgs cli_args = {.cfg = &cfg, .st = &st, .running = &running};
    run_cli(&cli_args);

    // Espera threads encerrarem
    if (i2c_started) {
        pthread_join(i2c_thread, NULL);
    }
    if (pub_started) {
        pthread_join(pub_thread, NULL);
    }

    mqtt_client_stop();
    state_destroy(&st);

    printf("Programa finalizado.\n");
    return EXIT_SUCCESS;
}
