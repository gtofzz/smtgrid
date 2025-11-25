#ifndef CONFIG_H
#define CONFIG_H

#include <pthread.h>
#include <stdbool.h>

// Estrutura de configuração compartilhada entre as threads.
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

void config_init_defaults(Config *cfg);
void config_print(const Config *cfg);
bool config_load_mqtt_from_file(Config *cfg, const char *path);

// Funções utilitárias para alterar configurações em tempo de execução
// de maneira simples e minimamente segura (com mutex interno).
void config_set_ids(Config *cfg, int id_no, int id_subno);
void config_set_i2c_address(Config *cfg, int address);
void config_set_pub_period(Config *cfg, double seconds);
void config_set_i2c_period(Config *cfg, double seconds);

// Cria uma cópia segura da configuração atual.
void config_copy(Config *dest, const Config *src);

#endif // CONFIG_H
