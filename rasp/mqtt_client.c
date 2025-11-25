#include "mqtt_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static struct mosquitto *g_mosq = NULL;
static State *g_state = NULL;
static Config *g_cfg = NULL;
static atomic_bool *g_running = NULL;
static pthread_t mqtt_thread;

static int parse_duty_from_payload(const char *payload, int payloadlen) {
    if (!payload) {
        return -1;
    }
    // Busca a substring "duty" e tenta ler o inteiro subsequente.
    if (!strstr(payload, "duty")) {
        return -1;
    }
    int duty = -1;
    for (int i = 0; i < payloadlen; ++i) {
        if (payload[i] == ':' || payload[i] == '=') {
            int read = sscanf(&payload[i + 1], "%d", &duty);
            if (read == 1) {
                break;
            }
        }
    }
    if (duty < 0) {
        duty = 0;
    }
    if (duty > 100) {
        duty = 100;
    }
    return duty;
}

static void on_connect(struct mosquitto *mosq, void *userdata, int rc) {
    (void)userdata;
    if (rc != 0) {
        fprintf(stderr, "MQTT: falha ao conectar, rc=%d\n", rc);
        if (g_state) {
            state_set_mqtt_error(g_state, "Erro de conexão MQTT");
        }
        return;
    }
    printf("MQTT conectado, assinando %s\n", g_cfg->cmd_luz_topic);
    mosquitto_subscribe(mosq, NULL, g_cfg->cmd_luz_topic, 1);
    state_clear_mqtt_error(g_state);
}

static void on_disconnect(struct mosquitto *mosq, void *userdata, int rc) {
    (void)mosq;
    (void)userdata;
    fprintf(stderr, "MQTT desconectado (rc=%d)\n", rc);
    if (g_state) {
        state_set_mqtt_error(g_state, "MQTT desconectado, tentando reconectar");
    }
}

static void on_message(struct mosquitto *mosq, void *userdata,
                       const struct mosquitto_message *msg) {
    (void)mosq;
    (void)userdata;
    if (!msg || !msg->payload) {
        return;
    }
    printf("MQTT msg recebida no tópico %s: %s\n", msg->topic,
           (char *)msg->payload);
    int duty = parse_duty_from_payload((const char *)msg->payload, msg->payloadlen);
    if (duty >= 0) {
        printf("Atualizando duty_req para %d%% via MQTT\n", duty);
        state_set_duty_req(g_state, duty);
        state_clear_mqtt_error(g_state);
    }
}

static void *mqtt_loop_thread(void *arg) {
    (void)arg;
    while (atomic_load(g_running)) {
        int rc = mosquitto_loop(g_mosq, 1000, 1);
        if (rc != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "mosquitto_loop erro: %s\n", mosquitto_strerror(rc));
            state_set_mqtt_error(g_state, mosquitto_strerror(rc));
            mosquitto_reconnect(g_mosq);
            struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

int mqtt_client_start(const MqttClientArgs *args) {
    if (!args || !args->cfg || !args->st || !args->running) {
        return -1;
    }
    g_cfg = args->cfg;
    g_state = args->st;
    g_running = args->running;

    mosquitto_lib_init();

    char client_id[128];
    snprintf(client_id, sizeof(client_id), "%s-%d", g_cfg->client_id_base,
             g_cfg->IDNo);
    g_mosq = mosquitto_new(client_id, true, NULL);
    if (!g_mosq) {
        fprintf(stderr, "Erro ao criar cliente mosquitto\n");
        return -1;
    }

    mosquitto_threaded_set(g_mosq, true);
    mosquitto_connect_callback_set(g_mosq, on_connect);
    mosquitto_disconnect_callback_set(g_mosq, on_disconnect);
    mosquitto_message_callback_set(g_mosq, on_message);

    int rc = mosquitto_connect(g_mosq, g_cfg->broker_address, g_cfg->broker_port,
                               60);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "Falha ao conectar ao broker: %s\n", mosquitto_strerror(rc));
        state_set_mqtt_error(g_state, mosquitto_strerror(rc));
        return -1;
    }

    if (pthread_create(&mqtt_thread, NULL, mqtt_loop_thread, NULL) != 0) {
        fprintf(stderr, "Não foi possível criar thread MQTT\n");
        mosquitto_destroy(g_mosq);
        g_mosq = NULL;
        return -1;
    }

    return 0;
}

void mqtt_client_stop(void) {
    if (!g_mosq) {
        return;
    }
    atomic_store(g_running, false);
    pthread_join(mqtt_thread, NULL);
    mosquitto_disconnect(g_mosq);
    mosquitto_destroy(g_mosq);
    g_mosq = NULL;
    mosquitto_lib_cleanup();
}

struct mosquitto *mqtt_get_client(void) { return g_mosq; }

int mqtt_publish_sensores(const Config *cfg, State *st, struct mosquitto *mosq,
                          int temp_c, int umid, int pwm) {
    if (!cfg || !st || !mosq) {
        return -1;
    }
    char payload[256];
    time_t now = time(NULL);
    snprintf(payload, sizeof(payload),
             "{\"IDNo\":%d,\"IDsubno\":%d,\"Temp\":%d,\"Umid\":%d,\"PWM\":%d,\"timestamp\":%ld}",
             cfg->IDNo, cfg->IDsubno, temp_c, umid, pwm, (long)now);
    int rc = mosquitto_publish(mosq, NULL, cfg->sensores_topic, strlen(payload),
                               payload, 1, false);
    if (rc != MOSQ_ERR_SUCCESS) {
        char errbuf[128];
        snprintf(errbuf, sizeof(errbuf), "Pub sensores falhou: %s",
                 mosquitto_strerror(rc));
        state_set_mqtt_error(st, errbuf);
    }
    return rc;
}

int mqtt_publish_status(const Config *cfg, State *st, struct mosquitto *mosq,
                        const char *status, const char *msg) {
    if (!cfg || !st || !mosq) {
        return -1;
    }
    char payload[256];
    time_t now = time(NULL);
    snprintf(payload, sizeof(payload),
             "{\"IDNo\":%d,\"IDsubno\":%d,\"status\":\"%s\",\"msg\":\"%s\",\"timestamp\":%ld}",
             cfg->IDNo, cfg->IDsubno, status ? status : "ok",
             msg ? msg : "", (long)now);
    int rc = mosquitto_publish(mosq, NULL, cfg->status_topic, strlen(payload),
                               payload, 1, false);
    if (rc != MOSQ_ERR_SUCCESS) {
        char errbuf[128];
        snprintf(errbuf, sizeof(errbuf), "Pub status falhou: %s",
                 mosquitto_strerror(rc));
        state_set_mqtt_error(st, errbuf);
    }
    return rc;
}
