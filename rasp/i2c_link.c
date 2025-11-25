#include "i2c_link.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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
        fprintf(stderr, "Leitura I2C incompleta (%zd bytes)\n", r);
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
        state_set_i2c_error(st, "Dados inválidos recebidos do STM32");
        return;
    }
    state_set_feedback(st, pwm_aplicado, temp_c, umid);
    state_clear_i2c_error(st);
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
    }

    while (atomic_load(args->running)) {
        config_copy(&cfg_local, args->cfg);

        if (fd < 0) {
            fd = open_i2c_device(cfg_local.i2c_device);
            if (fd < 0) {
                state_set_i2c_error(args->st, "Erro I2C: não abre /dev/i2c-1");
                usleep((useconds_t)(cfg_local.i2c_period_s * 1e6));
                continue;
            }
        }

        if (set_slave_address(fd, cfg_local.i2c_address) != 0) {
            state_set_i2c_error(args->st, "Erro ao configurar endereço I2C");
            usleep((useconds_t)(cfg_local.i2c_period_s * 1e6));
            continue;
        }

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

        ssize_t w = write(fd, tx, sizeof(tx));
        if (w != (ssize_t)sizeof(tx)) {
            char errmsg[128];
            snprintf(errmsg, sizeof(errmsg), "Erro I2C: escrita %zd/2 bytes", w);
            state_set_i2c_error(args->st, errmsg);
            usleep((useconds_t)(cfg_local.i2c_period_s * 1e6));
            continue;
        }

        int temp_cent = 0, umid_cent = 0, pwm_aplicado = 0;
        if (read_feedback(fd, &temp_cent, &umid_cent, &pwm_aplicado) != 0) {
            state_set_i2c_error(args->st, "Erro I2C: leitura de feedback");
            usleep((useconds_t)(cfg_local.i2c_period_s * 1e6));
            continue;
        }

        validate_and_update(args->st, temp_cent, umid_cent, pwm_aplicado);

        usleep((useconds_t)(cfg_local.i2c_period_s * 1e6));
    }

    if (fd >= 0) {
        close(fd);
    }
    return NULL;
}
