#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <mosquitto.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static atomic_bool running = true;

static void handle_sigint(int sig) {
    (void)sig;
    atomic_store(&running, false);
}

// ========================= Config =========================
typedef struct {
    char broker_address[128];
    int broker_port;
    char client_id_base[64];
    char cmd_luz_topic[64];
    char sensores_topic[64];
    char status_topic[64];

    char i2c_device[32];
    int i2c_address;

    int IDNo;
    int IDsubno;

    double pub_period_s;
    double i2c_period_s;

    pthread_mutex_t lock;
} Config;

static void config_init_defaults(Config *cfg) {
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    pthread_mutex_init(&cfg->lock, NULL);

    snprintf(cfg->broker_address, sizeof(cfg->broker_address), "127.0.0.1");
    cfg->broker_port = 1883;
    snprintf(cfg->client_id_base, sizeof(cfg->client_id_base), "raspi-no-i2c");
    snprintf(cfg->cmd_luz_topic, sizeof(cfg->cmd_luz_topic), "cmd/luz");
    snprintf(cfg->sensores_topic, sizeof(cfg->sensores_topic), "cmd/sensores");
    snprintf(cfg->status_topic, sizeof(cfg->status_topic), "cmd/status");

    snprintf(cfg->i2c_device, sizeof(cfg->i2c_device), "/dev/i2c-1");
    cfg->i2c_address = 0x28;

    cfg->IDNo = 1;
    cfg->IDsubno = 1;

    cfg->pub_period_s = 1.0;
    cfg->i2c_period_s = 0.5;
}

static bool config_load_mqtt_from_file(Config *cfg, const char *path) {
    if (!cfg || !path) {
        return false;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        return false;
    }

    char line[256];
    bool updated = false;
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        char value[128];
        if (sscanf(line, " %63[^=]=%127s", key, value) != 2) {
            continue;
        }

        if (strcmp(key, "broker_address") == 0) {
            pthread_mutex_lock(&cfg->lock);
            snprintf(cfg->broker_address, sizeof(cfg->broker_address), "%s", value);
            pthread_mutex_unlock(&cfg->lock);
            updated = true;
        } else if (strcmp(key, "broker_port") == 0) {
            int port = atoi(value);
            if (port > 0) {
                pthread_mutex_lock(&cfg->lock);
                cfg->broker_port = port;
                pthread_mutex_unlock(&cfg->lock);
                updated = true;
            }
        }
    }

    fclose(f);
    return updated;
}

static void config_print(const Config *cfg) {
    if (!cfg) {
        return;
    }
    pthread_mutex_lock((pthread_mutex_t *)&cfg->lock);
    printf("==== Configuração Atual ====%s", "\n");
    printf("Broker: %s:%d\n", cfg->broker_address, cfg->broker_port);
    printf("Client ID Base: %s\n", cfg->client_id_base);
    printf("Tópicos: cmd=%s sensores=%s status=%s\n", cfg->cmd_luz_topic,
           cfg->sensores_topic, cfg->status_topic);
    printf("I2C: dev=%s addr=0x%02X\n", cfg->i2c_device, cfg->i2c_address);
    printf("IDs: IDNo=%d IDsubno=%d\n", cfg->IDNo, cfg->IDsubno);
    printf("Períodos: pub=%.2fs i2c=%.2fs\n\n", cfg->pub_period_s,
           cfg->i2c_period_s);
    pthread_mutex_unlock((pthread_mutex_t *)&cfg->lock);
}

static void config_set_ids(Config *cfg, int id_no, int id_subno) {
    if (!cfg) {
        return;
    }
    pthread_mutex_lock(&cfg->lock);
    cfg->IDNo = id_no;
    cfg->IDsubno = id_subno;
    pthread_mutex_unlock(&cfg->lock);
}

static void config_set_i2c_address(Config *cfg, int address) {
    if (!cfg) {
        return;
    }
    pthread_mutex_lock(&cfg->lock);
    cfg->i2c_address = address;
    pthread_mutex_unlock(&cfg->lock);
}

static void config_set_pub_period(Config *cfg, double seconds) {
    if (!cfg) {
        return;
    }
    pthread_mutex_lock(&cfg->lock);
    cfg->pub_period_s = seconds;
    pthread_mutex_unlock(&cfg->lock);
}

static void config_set_i2c_period(Config *cfg, double seconds) {
    if (!cfg) {
        return;
    }
    pthread_mutex_lock(&cfg->lock);
    cfg->i2c_period_s = seconds;
    pthread_mutex_unlock(&cfg->lock);
}

static void config_copy(Config *dest, const Config *src) {
    if (!dest || !src) {
        return;
    }
    pthread_mutex_lock((pthread_mutex_t *)&src->lock);
    snprintf(dest->broker_address, sizeof(dest->broker_address), "%s",
             src->broker_address);
    dest->broker_port = src->broker_port;
    snprintf(dest->client_id_base, sizeof(dest->client_id_base), "%s",
             src->client_id_base);
    snprintf(dest->cmd_luz_topic, sizeof(dest->cmd_luz_topic), "%s",
             src->cmd_luz_topic);
    snprintf(dest->sensores_topic, sizeof(dest->sensores_topic), "%s",
             src->sensores_topic);
    snprintf(dest->status_topic, sizeof(dest->status_topic), "%s",
             src->status_topic);
    snprintf(dest->i2c_device, sizeof(dest->i2c_device), "%s", src->i2c_device);
    dest->i2c_address = src->i2c_address;
    dest->IDNo = src->IDNo;
    dest->IDsubno = src->IDsubno;
    dest->pub_period_s = src->pub_period_s;
    dest->i2c_period_s = src->i2c_period_s;
    pthread_mutex_unlock((pthread_mutex_t *)&src->lock);
}

// ========================= State =========================
typedef struct {
    int duty_req;
    int duty_aplicado;
    float temp_c;
    float umid;
    char last_i2c_error[256];
    char last_mqtt_error[256];
    pthread_mutex_t lock;
} State;

typedef struct {
    int duty_req;
    int duty_aplicado;
    float temp_c;
    float umid;
    char last_i2c_error[256];
    char last_mqtt_error[256];
} StateSnapshot;

static void state_init(State *st) {
    if (!st) {
        return;
    }
    memset(st, 0, sizeof(*st));
    pthread_mutex_init(&st->lock, NULL);
}

static void state_destroy(State *st) {
    if (!st) {
        return;
    }
    pthread_mutex_destroy(&st->lock);
}

static void state_set_duty_req(State *st, int duty) {
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

static void state_set_feedback(State *st, int duty_aplicado, float temp_c, float umid) {
    if (!st) {
        return;
    }
    pthread_mutex_lock(&st->lock);
    st->duty_aplicado = duty_aplicado;
    st->temp_c = temp_c;
    st->umid = umid;
    pthread_mutex_unlock(&st->lock);
}

static void state_set_i2c_error(State *st, const char *msg) {
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

static void state_clear_i2c_error(State *st) { state_set_i2c_error(st, ""); }

static void state_set_mqtt_error(State *st, const char *msg) {
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

static void state_clear_mqtt_error(State *st) { state_set_mqtt_error(st, ""); }

static void state_get_snapshot(State *st, StateSnapshot *out) {
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

// ========================= MQTT =========================
typedef struct {
    Config *cfg;
    State *st;
    atomic_bool *running;
} MqttClientArgs;

static struct mosquitto *g_mosq = NULL;
static State *g_state = NULL;
static Config *g_cfg = NULL;
static atomic_bool *g_running = NULL;
static pthread_t mqtt_thread;

static int parse_duty_from_payload(const char *payload, int payloadlen) {
    if (!payload) {
        return -1;
    }
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

static int mqtt_client_start(const MqttClientArgs *args) {
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

static void mqtt_client_stop(void) {
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

static struct mosquitto *mqtt_get_client(void) { return g_mosq; }

static int mqtt_publish_sensores(const Config *cfg, State *st, struct mosquitto *mosq,
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

static int mqtt_publish_status(const Config *cfg, State *st, struct mosquitto *mosq,
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

// ========================= I2C =========================
typedef struct {
    Config *cfg;
    State *st;
    atomic_bool *running;
} I2CThreadArgs;

static int set_slave_address(int fd, int addr) {
    if (ioctl(fd, I2C_SLAVE, addr) < 0) {
        fprintf(stderr, "Erro ao definir endereço I2C 0x%02X: %s\n", addr,
                strerror(errno));
        return -1;
    }
    return 0;
}

static int open_i2c_device(const char *path) {
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Não foi possível abrir %s: %s\n", path, strerror(errno));
    }
    return fd;
}

static int read_feedback(int fd, int *temp_cent, int *umid_cent, int *pwm_aplicado) {
    unsigned char rx[5] = {0};
    ssize_t r = read(fd, rx, sizeof(rx));
    if (r != (ssize_t)sizeof(rx)) {
        fprintf(stderr, "Leitura I2C incompleta (%zd bytes), esperado 5 (errno=%d)\n",
                r, errno);
        return -1;
    }
    int16_t temp_raw = (int16_t)(rx[1] << 8 | rx[0]);
    int16_t umid_raw = (int16_t)(rx[3] << 8 | rx[2]);
    *temp_cent = temp_raw;
    *umid_cent = umid_raw;
    *pwm_aplicado = rx[4];
    return 0;
}

static void validate_and_update(State *st, int temp_cent, int umid_cent,
                                int pwm_aplicado) {
    float temp_c = temp_cent / 100.0f;
    float umid = umid_cent / 100.0f;
    if (temp_c < -40.0f || temp_c > 125.0f || umid < 0.0f || umid > 100.0f) {
        fprintf(stderr,
                "I2C feedback fora da faixa: temp=%.2fC umid=%.2f%% pwm=%d\n",
                temp_c, umid, pwm_aplicado);
        state_set_i2c_error(st, "Dados inválidos recebidos do STM32");
        return;
    }
    printf("I2C feedback válido: temp=%.2fC umid=%.2f%% pwm_aplicado=%d\n",
           temp_c, umid, pwm_aplicado);
    state_set_feedback(st, pwm_aplicado, temp_c, umid);
    state_clear_i2c_error(st);
}

static void sleep_seconds(double seconds) {
    if (seconds <= 0) {
        return;
    }

    struct timespec req;
    req.tv_sec = (time_t)seconds;
    req.tv_nsec = (long)((seconds - req.tv_sec) * 1e9);

    while (nanosleep(&req, &req) == -1 && errno == EINTR) {
        continue;
    }
}

static void *i2c_thread_func(void *arg) {
    I2CThreadArgs *args = (I2CThreadArgs *)arg;
    if (!args || !args->cfg || !args->st || !args->running) {
        return NULL;
    }

    Config cfg_local;
    config_copy(&cfg_local, args->cfg);

    int fd = open_i2c_device(cfg_local.i2c_device);
    if (fd < 0) {
        state_set_i2c_error(args->st, "Falha ao abrir dispositivo I2C");
    } else {
        printf("I2C aberto em %s (fd=%d)\n", cfg_local.i2c_device, fd);
    }

    while (atomic_load(args->running)) {
        config_copy(&cfg_local, args->cfg);

        if (fd < 0) {
            fd = open_i2c_device(cfg_local.i2c_device);
            if (fd < 0) {
                state_set_i2c_error(args->st, "Erro I2C: não abre /dev/i2c-1");
                sleep_seconds(cfg_local.i2c_period_s);
                continue;
            }
        }

        if (set_slave_address(fd, cfg_local.i2c_address) != 0) {
            state_set_i2c_error(args->st, "Erro ao configurar endereço I2C");
            sleep_seconds(cfg_local.i2c_period_s);
            continue;
        }

        printf("I2C -> setando slave 0x%02X, comando PWM\n", cfg_local.i2c_address);

        StateSnapshot snap;
        state_get_snapshot(args->st, &snap);
        unsigned char tx[2];
        tx[0] = 0x01; // comando atualizar duty
        int duty = snap.duty_req;
        if (duty < 0) {
            duty = 0;
        }
        if (duty > 100) {
            duty = 100;
        }
        tx[1] = (unsigned char)duty;

        printf("I2C enviando comando 0x%02X com duty_req=%d%% (clamp=%d%%)\n",
               tx[0], snap.duty_req, duty);

        ssize_t w = write(fd, tx, sizeof(tx));
        if (w != (ssize_t)sizeof(tx)) {
            char errmsg[128];
            snprintf(errmsg, sizeof(errmsg), "Erro I2C: escrita %zd/2 bytes", w);
            state_set_i2c_error(args->st, errmsg);
            sleep_seconds(cfg_local.i2c_period_s);
            continue;
        }

        printf("I2C escrita OK (%zd bytes). Aguardando feedback...\n", w);

        int temp_cent = 0, umid_cent = 0, pwm_aplicado = 0;
        if (read_feedback(fd, &temp_cent, &umid_cent, &pwm_aplicado) != 0) {
            state_set_i2c_error(args->st, "Erro I2C: leitura de feedback");
            sleep_seconds(cfg_local.i2c_period_s);
            continue;
        }

        validate_and_update(args->st, temp_cent, umid_cent, pwm_aplicado);

        sleep_seconds(cfg_local.i2c_period_s);
    }

    if (fd >= 0) {
        close(fd);
    }
    return NULL;
}

// ========================= CLI =========================
typedef struct {
    Config *cfg;
    State *st;
    atomic_bool *running;
} CliArgs;

static void show_menu(void) {
    printf("\n==== Menu ====%s", "\n");
    printf("1) Mostrar estado atual\n");
    printf("2) Alterar IDNo/IDsubno\n");
    printf("3) Alterar endereço I2C do STM\n");
    printf("4) Alterar período de publicação (s)\n");
    printf("5) Alterar período de varredura I2C (s)\n");
    printf("6) Sair\n");
    printf("Escolha: ");
}

static void print_state(State *st) {
    StateSnapshot snap;
    state_get_snapshot(st, &snap);
    printf("Duty requisitado: %d%%\n", snap.duty_req);
    printf("Duty aplicado: %d%%\n", snap.duty_aplicado);
    printf("Temp: %.2f C\n", snap.temp_c);
    printf("Umidade: %.2f %%\n", snap.umid);
    printf("Erro I2C: %s\n",
           snap.last_i2c_error[0] ? snap.last_i2c_error : "nenhum");
    printf("Erro MQTT: %s\n",
           snap.last_mqtt_error[0] ? snap.last_mqtt_error : "nenhum");
}

static void run_cli(CliArgs *args) {
    if (!args || !args->cfg || !args->st || !args->running) {
        return;
    }

    int option = 0;
    while (atomic_load(args->running)) {
        show_menu();
        if (scanf("%d", &option) != 1) {
            printf("Entrada inválida.\n");
            int c;
            while ((c = getchar()) != '\n' && c != EOF) {
            }
            continue;
        }

        if (option == 1) {
            print_state(args->st);
        } else if (option == 2) {
            int idno, idsub;
            printf("Novo IDNo: ");
            scanf("%d", &idno);
            printf("Novo IDsubno: ");
            scanf("%d", &idsub);
            config_set_ids(args->cfg, idno, idsub);
            printf("IDs atualizados.\n");
        } else if (option == 3) {
            int addr;
            printf("Novo endereço I2C (ex 0x28): 0x");
            scanf("%x", &addr);
            config_set_i2c_address(args->cfg, addr);
            printf("Endereço I2C atualizado para 0x%02X.\n", addr);
        } else if (option == 4) {
            double period;
            printf("Novo período de publicação (s): ");
            scanf("%lf", &period);
            if (period < 0.1) {
                period = 0.1;
            }
            config_set_pub_period(args->cfg, period);
            printf("Período de publicação atualizado.\n");
        } else if (option == 5) {
            double period;
            printf("Novo período de varredura I2C (s): ");
            scanf("%lf", &period);
            if (period < 0.05) {
                period = 0.05;
            }
            config_set_i2c_period(args->cfg, period);
            printf("Período I2C atualizado.\n");
        } else if (option == 6) {
            printf("Encerrando...\n");
            atomic_store(args->running, false);
        } else {
            printf("Opção desconhecida.\n");
        }
    }
}

// ========================= Publisher =========================
typedef struct {
    Config *cfg;
    State *st;
    atomic_bool *running;
} PublisherArgs;

static void *publisher_thread_func(void *arg) {
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
            const char *detail = snap.last_i2c_error[0] ? snap.last_i2c_error : "";
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

// ========================= Main =========================
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
