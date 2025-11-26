// mqtt_server_sim.c
// "Servidor" MQTT simulado (faz o papel lógico do Node-RED).
//
// - Conecta ao broker MQTT (Mosquitto externo).
// - Envia comandos de PWM em cmd/luz.
// - Recebe leituras em cmd/sensores.
// - Recebe status em cmd/status.
// - Interface de terminal para:
//     * alterar PWM
//     * visualizar última leitura de sensores
//     * visualizar último status.
//
// Uso:
//   ./mqtt_server_sim              -> conecta em localhost:1883
//   ./mqtt_server_sim host         -> conecta em host:1883
//   ./mqtt_server_sim host port    -> conecta em host:port

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>

#include <mosquitto.h>

#ifndef MQTT_HOST_DEFAULT
#define MQTT_HOST_DEFAULT   "localhost"
#endif

#ifndef MQTT_PORT_DEFAULT
#define MQTT_PORT_DEFAULT   1883
#endif

#ifndef MQTT_KEEPALIVE
#define MQTT_KEEPALIVE      60
#endif

#define TOPIC_CMD_PWM       "cmd/luz"
#define TOPIC_SENSORES      "cmd/sensores"
#define TOPIC_STATUS        "cmd/status"

// ============================
// Controle de execução
// ============================

static volatile sig_atomic_t g_running = 1;

static void handle_sigint(int sig)
{
    (void)sig;
    g_running = 0;
}

// ============================
// LOG
// ============================

static void log_info(const char *msg)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M:%S", tm_info);
    printf("[%s] [INFO] %s\n", buf, msg);
    fflush(stdout);
}

static void log_error(const char *msg)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M:%S", tm_info);
    fprintf(stderr, "[%s] [ERRO] %s\n", buf, msg);
    fflush(stderr);
}

// ============================
// Estrutura para últimos dados
// ============================

typedef struct {
    float temp;
    float umid;
    int   pwm;
    int   duty_req;
    time_t last_update;

    char  status[16];      // "ok" ou "error"
    char  status_msg[128]; // última mensagem de status
} SensorData;

static SensorData g_data;
static pthread_mutex_t g_data_mutex = PTHREAD_MUTEX_INITIALIZER;

// ============================
// Parsing auxiliar
// ============================

// Busca um número float associado a uma chave, ex: "Temp":25.30
static bool parse_float_field(const char *payload,
                              const char *key,
                              float *out_val)
{
    char *p = strstr((char *)payload, key);
    if (!p) return false;

    p = strchr(p, ':');
    if (!p) return false;

    p++; // após ':'
    while (*p == ' ' || *p == '\t') p++;

    char *endptr;
    float v = strtof(p, &endptr);
    if (p == endptr) return false;

    if (out_val) *out_val = v;
    return true;
}

// Busca um número inteiro associado a uma chave, ex: "PWM":70
static bool parse_int_field(const char *payload,
                            const char *key,
                            int *out_val)
{
    char *p = strstr((char *)payload, key);
    if (!p) return false;

    p = strchr(p, ':');
    if (!p) return false;

    p++; // após ':'
    while (*p == ' ' || *p == '\t') p++;

    char *endptr;
    long v = strtol(p, &endptr, 10);
    if (p == endptr) return false;

    if (out_val) *out_val = (int)v;
    return true;
}

// Busca string em "status":"ok"
static bool parse_string_field(const char *payload,
                               const char *key,
                               char *out, size_t outlen)
{
    char *p = strstr((char *)payload, key);
    if (!p) return false;

    p = strchr(p, ':');
    if (!p) return false;

    p++; // após ':'
    while (*p == ' ' || *p == '\t') p++;

    if (*p != '\"') return false;
    p++; // após primeira aspa

    char *q = strchr(p, '\"');
    if (!q) return false;

    size_t len = (size_t)(q - p);
    if (len >= outlen) len = outlen - 1;

    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

// ============================
// Callbacks MQTT
// ============================

static void on_connect(struct mosquitto *mosq, void *userdata, int rc)
{
    (void)userdata;

    if (rc == 0) {
        log_info("Conectado ao broker MQTT.");

        mosquitto_subscribe(mosq, NULL, TOPIC_SENSORES, 0);
        mosquitto_subscribe(mosq, NULL, TOPIC_STATUS, 0);

        log_info("Assinatura em cmd/sensores e cmd/status enviada.");
    } else {
        char msg[80];
        snprintf(msg, sizeof(msg),
                 "Falha na conexão ao broker MQTT (rc=%d).", rc);
        log_error(msg);
    }
}

static void on_message(struct mosquitto *mosq, void *userdata,
                       const struct mosquitto_message *msg)
{
    (void)mosq;
    (void)userdata;

    if (!msg || !msg->topic || !msg->payload) return;

    char *payload = malloc(msg->payloadlen + 1);
    if (!payload) return;
    memcpy(payload, msg->payload, msg->payloadlen);
    payload[msg->payloadlen] = '\0';

    if (strcmp(msg->topic, TOPIC_SENSORES) == 0) {
        // Espera JSON: {"Temp":..,"Umid":..,"PWM":..,"DutyReq":..}
        float temp = 0.0f, umid = 0.0f;
        int pwm = 0, duty_req = 0;

        bool ok_temp = parse_float_field(payload, "Temp", &temp);
        bool ok_umid = parse_float_field(payload, "Umid", &umid);
        bool ok_pwm  = parse_int_field(payload,  "PWM",  &pwm);
        bool ok_dreq = parse_int_field(payload,  "DutyReq", &duty_req);

        if (ok_temp || ok_umid || ok_pwm || ok_dreq) {
            pthread_mutex_lock(&g_data_mutex);
            if (ok_temp) g_data.temp = temp;
            if (ok_umid) g_data.umid = umid;
            if (ok_pwm)  g_data.pwm  = pwm;
            if (ok_dreq) g_data.duty_req = duty_req;
            g_data.last_update = time(NULL);
            pthread_mutex_unlock(&g_data_mutex);

            char dbg[256];
            snprintf(dbg, sizeof(dbg),
                     "[MQTT-SERVER-SIM] SENSORES: %s", payload);
            log_info(dbg);
        } else {
            char dbg[256];
            snprintf(dbg, sizeof(dbg),
                     "[MQTT-SERVER-SIM] Payload de sensores não reconhecido: %s",
                     payload);
            log_error(dbg);
        }

    } else if (strcmp(msg->topic, TOPIC_STATUS) == 0) {
        // Espera algo como:
        // {"status":"ok"} ou {"status":"error","msg":"..."}
        char status[16] = "";
        char msg_txt[128] = "";

        bool ok_status = parse_string_field(payload, "status",
                                            status, sizeof(status));
        parse_string_field(payload, "msg", msg_txt, sizeof(msg_txt));

        if (ok_status) {
            pthread_mutex_lock(&g_data_mutex);
            strncpy(g_data.status, status, sizeof(g_data.status));
            g_data.status[sizeof(g_data.status) - 1] = '\0';
            strncpy(g_data.status_msg, msg_txt, sizeof(g_data.status_msg));
            g_data.status_msg[sizeof(g_data.status_msg) - 1] = '\0';
            pthread_mutex_unlock(&g_data_mutex);

            char dbg[256];
            snprintf(dbg, sizeof(dbg),
                     "[MQTT-SERVER-SIM] STATUS: %s", payload);
            log_info(dbg);
        } else {
            char dbg[256];
            snprintf(dbg, sizeof(dbg),
                     "[MQTT-SERVER-SIM] Payload de status não reconhecido: %s",
                     payload);
            log_error(dbg);
        }
    }

    free(payload);
}

// ============================
// Funções auxiliares
// ============================

static void show_menu(void)
{
    printf("\n========== MQTT SERVER SIM ==========\n");
    printf("1) Enviar novo PWM\n");
    printf("2) Mostrar última leitura de sensores\n");
    printf("3) Mostrar último status\n");
    printf("4) Sair\n");
    printf("Escolha: ");
    fflush(stdout);
}

static void show_sensors(void)
{
    SensorData local;
    pthread_mutex_lock(&g_data_mutex);
    local = g_data;
    pthread_mutex_unlock(&g_data_mutex);

    if (local.last_update == 0) {
        printf("Ainda não foram recebidos dados de sensores.\n");
        return;
    }

    char buf[64];
    struct tm *tm_info = localtime(&local.last_update);
    strftime(buf, sizeof(buf), "%H:%M:%S", tm_info);

    printf("----- Últimos sensores -----\n");
    printf("Temp     : %.2f °C\n", local.temp);
    printf("Umid     : %.2f %%\n", local.umid);
    printf("PWM      : %d %%\n", local.pwm);
    printf("Duty Req : %d %%\n", local.duty_req);
    printf("Atualizado em: %s\n", buf);
}

static void show_status(void)
{
    SensorData local;
    pthread_mutex_lock(&g_data_mutex);
    local = g_data;
    pthread_mutex_unlock(&g_data_mutex);

    printf("----- Último status -----\n");
    if (local.status[0] == '\0') {
        printf("Sem status recebido ainda.\n");
        return;
    }

    printf("Status: %s\n", local.status);
    if (local.status_msg[0] != '\0') {
        printf("Msg   : %s\n", local.status_msg);
    }
}

// ============================
// MAIN
// ============================

int main(int argc, char *argv[])
{
    const char *mqtt_host = MQTT_HOST_DEFAULT;
    int mqtt_port = MQTT_PORT_DEFAULT;

    if (argc >= 2) {
        mqtt_host = argv[1];
    }
    if (argc >= 3) {
        mqtt_port = atoi(argv[2]);
        if (mqtt_port <= 0 || mqtt_port > 65535) {
            log_error("Porta MQTT invalida.");
            return 1;
        }
    }

    signal(SIGINT, handle_sigint);

    // Inicializa estrutura de dados
    memset(&g_data, 0, sizeof(g_data));
    strncpy(g_data.status, "desconhecido", sizeof(g_data.status) - 1);

    // MQTT
    mosquitto_lib_init();

    struct mosquitto *mosq = mosquitto_new("mqtt_server_sim", true, NULL);
    if (!mosq) {
        log_error("Falha ao criar instância MQTT.");
        mosquitto_lib_cleanup();
        return 1;
    }

    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_message_callback_set(mosq, on_message);

    int rc = mosquitto_connect(mosq, mqtt_host, mqtt_port, MQTT_KEEPALIVE);
    if (rc != MOSQ_ERR_SUCCESS) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "Erro ao conectar no broker MQTT: %s",
                 mosquitto_strerror(rc));
        log_error(msg);
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return 1;
    }

    // Loop de rede em thread própria
    rc = mosquitto_loop_start(mosq);
    if (rc != MOSQ_ERR_SUCCESS) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "Erro ao iniciar loop MQTT: %s",
                 mosquitto_strerror(rc));
        log_error(msg);
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return 1;
    }

    log_info("mqtt_server_sim em execução. Use o menu para interagir (Ctrl+C também sai).");

    // Loop de interação com o usuário
    char line[64];

    while (g_running) {
        show_menu();

        if (!fgets(line, sizeof(line), stdin)) {
            // EOF (Ctrl+D) ou erro
            break;
        }

        if (!g_running) break;

        int opcao = atoi(line);

        if (opcao == 1) {
            printf("Digite o PWM desejado (0-100): ");
            fflush(stdout);

            if (!fgets(line, sizeof(line), stdin)) {
                break;
            }
            int duty = atoi(line);
            if (duty < 0) duty = 0;
            if (duty > 100) duty = 100;

            // Payload simples: inteiro em texto ("73")
            char payload[16];
            snprintf(payload, sizeof(payload), "%d", duty);

            int rc_pub = mosquitto_publish(mosq, NULL, TOPIC_CMD_PWM,
                                           (int)strlen(payload), payload,
                                           0, false);
            if (rc_pub != MOSQ_ERR_SUCCESS) {
                char msg[128];
                snprintf(msg, sizeof(msg),
                         "Falha ao publicar PWM: %s",
                         mosquitto_strerror(rc_pub));
                log_error(msg);
            } else {
                char dbg[128];
                snprintf(dbg, sizeof(dbg),
                         "[MQTT-SERVER-SIM] Publicado PWM=%d em %s",
                         duty, TOPIC_CMD_PWM);
                log_info(dbg);
            }

        } else if (opcao == 2) {
            show_sensors();
        } else if (opcao == 3) {
            show_status();
        } else if (opcao == 4) {
            log_info("Saindo por comando do usuário.");
            break;
        } else {
            printf("Opção inválida.\n");
        }
    }

    g_running = 0;

    mosquitto_loop_stop(mosq, true);
    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();

    log_info("mqtt_server_sim finalizado.");
    return 0;
}

