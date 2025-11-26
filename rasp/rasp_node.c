// rasp_node.c
// Nó Raspberry: ponte MQTT <-> I2C (real ou simulada por thread interna).
//
// - Compilar com -DSIM_I2C para usar I2C SIMULADA:
//     * Uma thread interna simula o "STM": recebe duty e gera Temp/Umid/PWM.
//     * Comunicação entre a thread de I2C e a thread simulada é por memória
//       compartilhada (variáveis globais protegidas por mutex).
//
// - Compilar SEM -DSIM_I2C para usar I2C REAL:
//     * Usa /dev/i2c-1 como master I2C, falando com o STM no endereço I2C_STM_ADDRESS.
//
// MQTT é usado APENAS entre o Raspberry (este programa) e o servidor (Node-RED / mqtt_sim).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <stdbool.h>
#include <signal.h>

#include <mosquitto.h>

#ifndef SIM_I2C
    #include <fcntl.h>
    #include <sys/ioctl.h>
    #include <linux/i2c-dev.h>
#endif

// ============================
// CONFIGURAÇÕES GERAIS
// ============================

// MQTT
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

// I2C real
#ifndef SIM_I2C
    #ifndef I2C_DEVICE_DEFAULT
    #define I2C_DEVICE_DEFAULT  "/dev/i2c-1"
    #endif

    #ifndef I2C_STM_ADDRESS
    #define I2C_STM_ADDRESS     0x20   // ajuste depois se necessário
    #endif
#endif

// Períodos das threads
#define I2C_THREAD_PERIOD_MS    200     // 200 ms
#define MQTT_PUB_PERIOD_MS      1000    // 1 s

// ============================
// FUNÇÃO DE ESPERA (msleep)
// ============================

static void msleep(int ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

// ============================
// ESTADO COMPARTILHADO PRINCIPAL
// ============================

typedef struct {
    int   duty_requested;   // 0-100, vindo do MQTT
    int   duty_applied;     // 0-100, feedback do STM/simulador
    float temperature;      // °C
    float humidity;         // %RH
    int   last_i2c_error;   // 0 = OK, !=0 indica erro
    char  last_error_msg[128];
    time_t last_update;     // timestamp da última leitura válida
} SystemState;

static SystemState g_state;
static pthread_mutex_t g_state_mutex = PTHREAD_MUTEX_INITIALIZER;

// Controle de execução
static volatile sig_atomic_t g_running = 1;

// Handler para Ctrl+C
static void handle_sigint(int sig)
{
    (void)sig;
    g_running = 0;
}

// ============================
// LOG SIMPLES
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
// CAMADA DE I2C (ABSTRAÇÃO)
// ============================
//
// i2c_init()
// i2c_send_duty(duty)
// i2c_read_sensors(&temp, &hum, &pwm)
// i2c_close()
//
// Implementada de duas formas:
//  - SIM_I2C: usa thread interna que simula o STM via memória compartilhada.
//  - REAL   : usa /dev/i2c-1.
//
// O restante do código NÃO sabe qual modo está sendo usado.

#ifdef SIM_I2C

// ---------- MODO SIMULADO (thread interna "fake STM") ----------

// Estado compartilhado entre a thread "fake STM" e a thread de I2C
typedef struct {
    int   duty;        // duty "comandado" pelo master
    float temperature; // °C
    float humidity;    // %RH
} SimI2CState;

static SimI2CState g_sim_i2c_state;
static pthread_mutex_t g_sim_i2c_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_sim_i2c_thread;
static int g_sim_i2c_thread_started = 0;

// Atualiza o ambiente em um passo de tempo fixo (dt = 0.1 s)
static void sim_i2c_update_step(void)
{
    const float dt  = 0.1f;  // 100 ms
    const float tau = 5.0f;  // constante de tempo "térmica"

    pthread_mutex_lock(&g_sim_i2c_mutex);

    int   pwm   = g_sim_i2c_state.duty;
    float temp  = g_sim_i2c_state.temperature;
    float umid  = g_sim_i2c_state.humidity;

    // Alvos em função do PWM:
    //   temp_target = 24 + 6*(pwm/100)  => 24..30 °C
    //   umid_target = 60 - 15*(pwm/100) => 60..45 %
    float target_temp = 24.0f + 6.0f  * (pwm / 100.0f);
    float target_umid = 60.0f - 15.0f * (pwm / 100.0f);

    float alpha = dt / tau;
    if (alpha > 1.0f) alpha = 1.0f;

    temp += (target_temp - temp) * alpha;
    umid += (target_umid - umid) * alpha;

    // Ruído pequeno
    float noise_t = ((rand() % 1001) / 1000.0f - 0.5f) * 0.1f; // +-0.05
    float noise_h = ((rand() % 1001) / 1000.0f - 0.5f) * 0.2f; // +-0.1

    temp += noise_t;
    umid += noise_h;

    // Limites razoáveis
    if (temp < 15.0f) temp = 15.0f;
    if (temp > 40.0f) temp = 40.0f;
    if (umid < 20.0f) umid = 20.0f;
    if (umid > 90.0f) umid = 90.0f;

    g_sim_i2c_state.temperature = temp;
    g_sim_i2c_state.humidity    = umid;

    pthread_mutex_unlock(&g_sim_i2c_mutex);
}

// Thread que simula o STM: reage ao duty e atualiza Temp/Umid
static void *sim_i2c_thread_func(void *arg)
{
    (void)arg;
    log_info("Thread I2C-SIM (fake STM) iniciada.");

    // Inicializa ambiente
    pthread_mutex_lock(&g_sim_i2c_mutex);
    g_sim_i2c_state.duty        = 0;
    g_sim_i2c_state.temperature = 25.0f;
    g_sim_i2c_state.humidity    = 55.0f;
    pthread_mutex_unlock(&g_sim_i2c_mutex);

    while (g_running) {
        sim_i2c_update_step();
        msleep(100); // 100 ms
    }

    log_info("Thread I2C-SIM (fake STM) finalizada.");
    return NULL;
}

// Inicializa a simulação de I2C
static int i2c_init_sim(void)
{
    srand((unsigned)time(NULL));

    int ret = pthread_create(&g_sim_i2c_thread, NULL,
                             sim_i2c_thread_func, NULL);
    if (ret != 0) {
        perror("pthread_create sim_i2c_thread");
        return -1;
    }

    g_sim_i2c_thread_started = 1;
    log_info("I2C simulado inicializado (thread fake STM).");
    return 0;
}

// "Envia" duty para o STM simulado (apenas escreve na memória compartilhada)
static int i2c_send_duty(int duty)
{
    if (duty < 0) duty = 0;
    if (duty > 100) duty = 100;

    pthread_mutex_lock(&g_sim_i2c_mutex);
    g_sim_i2c_state.duty = duty;
    pthread_mutex_unlock(&g_sim_i2c_mutex);

    char dbg[128];
    snprintf(dbg, sizeof(dbg),
             "[I2C-SIM] SET_PWM duty=%d (memória compartilhada)", duty);
    log_info(dbg);

    return 0; // sem erro
}

// "Lê" sensores do STM simulado (copia da memória compartilhada)
static int i2c_read_sensors(float *temp, float *hum, int *pwm)
{
    pthread_mutex_lock(&g_sim_i2c_mutex);
    float t = g_sim_i2c_state.temperature;
    float h = g_sim_i2c_state.humidity;
    int   d = g_sim_i2c_state.duty;
    pthread_mutex_unlock(&g_sim_i2c_mutex);

    if (temp) *temp = t;
    if (hum)  *hum  = h;
    if (pwm)  *pwm  = d;  // aqui podemos dizer que PWM aplicado = duty

    char dbg[160];
    snprintf(dbg, sizeof(dbg),
             "[I2C-SIM] READ -> Temp=%.2f, Umid=%.2f, PWM=%d",
             t, h, d);
    log_info(dbg);

    return 0; // sem erro
}

static void i2c_close_sim(void)
{
    if (g_sim_i2c_thread_started) {
        // g_running já será 0 quando main estiver encerrando
        pthread_join(g_sim_i2c_thread, NULL);
        g_sim_i2c_thread_started = 0;
    }
    log_info("I2C simulado encerrado.");
}

#else  // ---------- MODO REAL (I2C FÍSICO) ----------

static int g_i2c_fd = -1;

static int i2c_init_real(const char *device, int addr)
{
    g_i2c_fd = open(device, O_RDWR);
    if (g_i2c_fd < 0) {
        perror("open i2c");
        return -1;
    }

    if (ioctl(g_i2c_fd, I2C_SLAVE, addr) < 0) {
        perror("ioctl I2C_SLAVE");
        close(g_i2c_fd);
        g_i2c_fd = -1;
        return -1;
    }

    log_info("I2C real inicializado (/dev/i2c-1).");
    return 0;
}

// Protocolo simples: escreve [CMD, DUTY], lê [T_H, T_L, H_H, H_L, PWM]
// Temperatura/umidade em centésimos (x100).
static int i2c_send_duty(int duty)
{
    if (g_i2c_fd < 0) {
        return -1;
    }

    if (duty < 0) duty = 0;
    if (duty > 100) duty = 100;

    unsigned char buf[2];
    buf[0] = 0x01;        // CMD: SET_PWM
    buf[1] = (unsigned char)duty;

    ssize_t w = write(g_i2c_fd, buf, 2);
    if (w != 2) {
        perror("write i2c SET_PWM");
        return -1;
    }

    char msg[80];
    snprintf(msg, sizeof(msg), "[I2C-REAL] SET_PWM duty=%d enviado.", duty);
    log_info(msg);

    return 0;
}

static int i2c_read_sensors(float *temp, float *hum, int *pwm)
{
    if (g_i2c_fd < 0) {
        return -1;
    }

    unsigned char buf[5];
    ssize_t r = read(g_i2c_fd, buf, 5);
    if (r != 5) {
        perror("read i2c sensores");
        return -1;
    }

    unsigned short t_raw = (buf[0] << 8) | buf[1];
    unsigned short h_raw = (buf[2] << 8) | buf[3];
    int p = buf[4];

    if (temp) *temp = t_raw / 100.0f;
    if (hum)  *hum  = h_raw / 100.0f;
    if (pwm)  *pwm  = p;

    char msg[160];
    snprintf(msg, sizeof(msg),
             "[I2C-REAL] READ -> Temp=%.2f, Umid=%.2f, PWM=%d",
             t_raw / 100.0f, h_raw / 100.0f, p);
    log_info(msg);

    return 0;
}

static void i2c_close_real(void)
{
    if (g_i2c_fd >= 0) {
        close(g_i2c_fd);
        g_i2c_fd = -1;
    }
    log_info("I2C real fechado.");
}

#endif

// ---------- Wrap genérico (usa uma das implementações acima) ----------

static int i2c_init(void)
{
#ifdef SIM_I2C
    return i2c_init_sim();
#else
    return i2c_init_real(I2C_DEVICE_DEFAULT, I2C_STM_ADDRESS);
#endif
}

static void i2c_close(void)
{
#ifdef SIM_I2C
    i2c_close_sim();
#else
    i2c_close_real();
#endif
}

// ============================
// MQTT
// ============================

static struct mosquitto *g_mosq = NULL;

// Callback de conexão
static void on_connect(struct mosquitto *mosq, void *userdata, int rc)
{
    (void)userdata;
    if (rc == 0) {
        log_info("Conectado ao broker MQTT.");
        // Assinar tópico de comando PWM
        mosquitto_subscribe(mosq, NULL, TOPIC_CMD_PWM, 0);
        log_info("Assinatura em " TOPIC_CMD_PWM " enviada.");
    } else {
        char msg[80];
        snprintf(msg, sizeof(msg), "Falha na conexão ao broker MQTT (rc=%d).", rc);
        log_error(msg);
    }
}

// Extrai duty de um JSON simples {"duty": 73} OU do payload como inteiro "73"
static int parse_duty_from_payload(const char *payload)
{
    if (!payload) return -1;

    // 1) Tentar como inteiro puro
    char *endptr = NULL;
    long val = strtol(payload, &endptr, 10);
    if (endptr != payload && *endptr == '\0') {
        if (val < 0) val = 0;
        if (val > 100) val = 100;
        return (int)val;
    }

    // 2) Tentar encontrar "duty" em um JSON simples
    const char *p = strstr(payload, "duty");
    if (!p) p = strstr(payload, "PWM"); // fallback

    if (p) {
        // procurar ':' depois de "duty"
        const char *colon = strchr(p, ':');
        if (colon) {
            val = strtol(colon + 1, &endptr, 10);
            if (colon + 1 != endptr) {
                if (val < 0) val = 0;
                if (val > 100) val = 100;
                return (int)val;
            }
        }
    }

    return -1; // não conseguiu
}

// Callback de mensagem (MQTT)
static void on_message(struct mosquitto *mosq, void *userdata,
                       const struct mosquitto_message *msg)
{
    (void)mosq;
    (void)userdata;

    if (!msg || !msg->topic || !msg->payload) return;

    if (strcmp(msg->topic, TOPIC_CMD_PWM) == 0) {
        char *payload_str = malloc(msg->payloadlen + 1);
        if (!payload_str) return;
        memcpy(payload_str, msg->payload, msg->payloadlen);
        payload_str[msg->payloadlen] = '\0';

        int new_duty = parse_duty_from_payload(payload_str);
        if (new_duty >= 0) {
            pthread_mutex_lock(&g_state_mutex);
            g_state.duty_requested = new_duty;
            pthread_mutex_unlock(&g_state_mutex);

            char dbg[128];
            snprintf(dbg, sizeof(dbg),
                     "[MQTT] Recebido novo duty=%d (payload='%s')",
                     new_duty, payload_str);
            log_info(dbg);
        } else {
            char dbg[160];
            snprintf(dbg, sizeof(dbg),
                     "[MQTT] Payload de comando inválido: '%s'",
                     payload_str);
            log_error(dbg);
        }

        free(payload_str);
    }
}

// ============================
// THREAD: I2C
// ============================

static void *i2c_thread_func(void *arg)
{
    (void)arg;

    log_info("Thread I2C iniciada.");

    while (g_running) {
        int duty_local;
        pthread_mutex_lock(&g_state_mutex);
        duty_local = g_state.duty_requested;
        pthread_mutex_unlock(&g_state_mutex);

        // Envia duty
        int err = i2c_send_duty(duty_local);
        if (err != 0) {
            pthread_mutex_lock(&g_state_mutex);
            g_state.last_i2c_error = err;
            snprintf(g_state.last_error_msg,
                     sizeof(g_state.last_error_msg),
                     "Erro ao enviar duty via I2C.");
            pthread_mutex_unlock(&g_state_mutex);
        }

        // Lê sensores
        float t = 0.0f, h = 0.0f;
        int pwm_applied = 0;
        err = i2c_read_sensors(&t, &h, &pwm_applied);
        if (err == 0) {
            pthread_mutex_lock(&g_state_mutex);
            g_state.temperature   = t;
            g_state.humidity      = h;
            g_state.duty_applied  = pwm_applied;
            g_state.last_i2c_error = 0;
            g_state.last_error_msg[0] = '\0';
            g_state.last_update   = time(NULL);
            pthread_mutex_unlock(&g_state_mutex);
        } else {
            pthread_mutex_lock(&g_state_mutex);
            g_state.last_i2c_error = err;
            snprintf(g_state.last_error_msg,
                     sizeof(g_state.last_error_msg),
                     "Erro ao ler sensores via I2C.");
            pthread_mutex_unlock(&g_state_mutex);
        }

        msleep(I2C_THREAD_PERIOD_MS);
    }

    log_info("Thread I2C finalizada.");
    return NULL;
}

// ============================
// THREAD: MQTT PUBLISH
// ============================

static void *mqtt_pub_thread_func(void *arg)
{
    (void)arg;
    log_info("Thread MQTT-PUB iniciada.");

    char payload[256];

    while (g_running) {
        // copia estado
        int duty_req, duty_ap;
        float temp, hum;
        int last_err;
        char last_msg[128];

        pthread_mutex_lock(&g_state_mutex);
        duty_req = g_state.duty_requested;
        duty_ap  = g_state.duty_applied;
        temp     = g_state.temperature;
        hum      = g_state.humidity;
        last_err = g_state.last_i2c_error;
        strncpy(last_msg, g_state.last_error_msg, sizeof(last_msg));
        last_msg[sizeof(last_msg) - 1] = '\0';
        pthread_mutex_unlock(&g_state_mutex);

        // Monta JSON de sensores
        // Exemplo: {"Temp":25.30,"Umid":55.10,"PWM":70,"DutyReq":70}
        snprintf(payload, sizeof(payload),
                 "{\"Temp\":%.2f,\"Umid\":%.2f,\"PWM\":%d,\"DutyReq\":%d}",
                 temp, hum, duty_ap, duty_req);

        int rc = mosquitto_publish(g_mosq, NULL, TOPIC_SENSORES,
                                   (int)strlen(payload), payload, 0, false);
        if (rc != MOSQ_ERR_SUCCESS) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "Falha ao publicar sensores: %s",
                     mosquitto_strerror(rc));
            log_error(msg);
        } else {
            char dbg[256];
            snprintf(dbg, sizeof(dbg),
                     "[MQTT] Publicado em %s: %s",
                     TOPIC_SENSORES, payload);
            log_info(dbg);
        }

        // Publica status
        if (last_err != 0) {
            snprintf(payload, sizeof(payload),
                     "{\"status\":\"error\",\"msg\":\"%s\"}", last_msg);
        } else {
            snprintf(payload, sizeof(payload),
                     "{\"status\":\"ok\"}");
        }

        rc = mosquitto_publish(g_mosq, NULL, TOPIC_STATUS,
                               (int)strlen(payload), payload, 0, false);
        if (rc != MOSQ_ERR_SUCCESS) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "Falha ao publicar status: %s",
                     mosquitto_strerror(rc));
            log_error(msg);
        } else {
            char dbg[256];
            snprintf(dbg, sizeof(dbg),
                     "[MQTT] Publicado em %s: %s",
                     TOPIC_STATUS, payload);
            log_info(dbg);
        }

        msleep(MQTT_PUB_PERIOD_MS);
    }

    log_info("Thread MQTT-PUB finalizada.");
    return NULL;
}

// ============================
// MAIN
// ============================

int main(int argc, char *argv[])
{
    const char *mqtt_host = MQTT_HOST_DEFAULT;
    int mqtt_port = MQTT_PORT_DEFAULT;

    if (argc >= 2) {
        mqtt_host = argv[1]; // ex.: ./rasp_node localhost
    }
    if (argc >= 3) {
        mqtt_port = atoi(argv[2]); // ex.: ./rasp_node host 1883
    }

    signal(SIGINT, handle_sigint);

    // Estado inicial
    memset(&g_state, 0, sizeof(g_state));
    g_state.duty_requested = 0;
    g_state.duty_applied   = 0;
    g_state.temperature    = 0.0f;
    g_state.humidity       = 0.0f;
    g_state.last_i2c_error = 0;
    g_state.last_update    = time(NULL);

    // Inicializa I2C (real ou simulada)
    if (i2c_init() != 0) {
        log_error("Falha na inicialização do I2C.");
        return 1;
    }

    // Inicializa MQTT
    mosquitto_lib_init();

    g_mosq = mosquitto_new("rasp_node", true, NULL);
    if (!g_mosq) {
        log_error("Falha ao criar instância MQTT.");
        i2c_close();
        mosquitto_lib_cleanup();
        return 1;
    }

    mosquitto_connect_callback_set(g_mosq, on_connect);
    mosquitto_message_callback_set(g_mosq, on_message);

    int rc = mosquitto_connect(g_mosq, mqtt_host, mqtt_port, MQTT_KEEPALIVE);
    if (rc != MOSQ_ERR_SUCCESS) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "Erro ao conectar no broker MQTT: %s",
                 mosquitto_strerror(rc));
        log_error(msg);
        mosquitto_destroy(g_mosq);
        i2c_close();
        mosquitto_lib_cleanup();
        return 1;
    }

    // Inicia loop de rede do Mosquitto em thread própria
    rc = mosquitto_loop_start(g_mosq);
    if (rc != MOSQ_ERR_SUCCESS) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "Erro ao iniciar loop MQTT: %s",
                 mosquitto_strerror(rc));
        log_error(msg);
        mosquitto_destroy(g_mosq);
        i2c_close();
        mosquitto_lib_cleanup();
        return 1;
    }

    // Cria threads I2C e MQTT-PUB
    pthread_t tid_i2c, tid_mqtt_pub;
    if (pthread_create(&tid_i2c, NULL, i2c_thread_func, NULL) != 0) {
        perror("pthread_create i2c_thread");
        g_running = 0;
    }

    if (pthread_create(&tid_mqtt_pub, NULL, mqtt_pub_thread_func, NULL) != 0) {
        perror("pthread_create mqtt_pub_thread");
        g_running = 0;
    }

#ifdef SIM_I2C
    log_info("rasp_node em execução (MODO SIM_I2C). Pressione Ctrl+C para sair.");
#else
    log_info("rasp_node em execução (MODO I2C REAL). Pressione Ctrl+C para sair.");
#endif

    // Espera Ctrl+C
    while (g_running) {
        msleep(500);
    }

    log_info("Encerrando...");

    // Aguarda threads
    pthread_join(tid_i2c, NULL);
    pthread_join(tid_mqtt_pub, NULL);

    // Para loop MQTT
    mosquitto_loop_stop(g_mosq, true);
    mosquitto_disconnect(g_mosq);
    mosquitto_destroy(g_mosq);
    mosquitto_lib_cleanup();

    // Fecha I2C (simulada ou real)
    i2c_close();

    log_info("Finalizado.");
    return 0;
}

