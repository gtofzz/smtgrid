#include "i2c_link.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

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

static int write_full(int fd, const unsigned char *buf, size_t len) {
    size_t total = 0;

    while (total < len) {
        ssize_t w = write(fd, buf + total, len - total);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct timespec retry_delay = {.tv_sec = 0, .tv_nsec = 1000000};
                nanosleep(&retry_delay, NULL);
                continue;
            }
            return -1;
        }
        if (w == 0) {
            errno = EIO;
            return -1;
        }

        total += (size_t)w;
    }

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

void *i2c_thread_func(void *arg) {
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

        if (write_full(fd, tx, sizeof(tx)) != 0) {
            char errmsg[128];
            snprintf(errmsg, sizeof(errmsg), "Erro I2C: falha na escrita (%s)",
                     strerror(errno));
            state_set_i2c_error(args->st, errmsg);
            sleep_seconds(cfg_local.i2c_period_s);
            continue;
        }

        printf("I2C escrita OK (%zu bytes). Aguardando feedback...\n", sizeof(tx));

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
