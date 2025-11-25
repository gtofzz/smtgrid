#include "config.h"

#include <stdio.h>
#include <string.h>

void config_init_defaults(Config *cfg) {
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

void config_print(const Config *cfg) {
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

void config_set_ids(Config *cfg, int id_no, int id_subno) {
    if (!cfg) {
        return;
    }
    pthread_mutex_lock(&cfg->lock);
    cfg->IDNo = id_no;
    cfg->IDsubno = id_subno;
    pthread_mutex_unlock(&cfg->lock);
}

void config_set_i2c_address(Config *cfg, int address) {
    if (!cfg) {
        return;
    }
    pthread_mutex_lock(&cfg->lock);
    cfg->i2c_address = address;
    pthread_mutex_unlock(&cfg->lock);
}

void config_set_pub_period(Config *cfg, double seconds) {
    if (!cfg) {
        return;
    }
    pthread_mutex_lock(&cfg->lock);
    cfg->pub_period_s = seconds;
    pthread_mutex_unlock(&cfg->lock);
}

void config_set_i2c_period(Config *cfg, double seconds) {
    if (!cfg) {
        return;
    }
    pthread_mutex_lock(&cfg->lock);
    cfg->i2c_period_s = seconds;
    pthread_mutex_unlock(&cfg->lock);
}

void config_copy(Config *dest, const Config *src) {
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
    snprintf(dest->i2c_device, sizeof(dest->i2c_device), "%s",
             src->i2c_device);
    dest->i2c_address = src->i2c_address;
    dest->IDNo = src->IDNo;
    dest->IDsubno = src->IDsubno;
    dest->pub_period_s = src->pub_period_s;
    dest->i2c_period_s = src->i2c_period_s;
    pthread_mutex_unlock((pthread_mutex_t *)&src->lock);
}
