#include "cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    printf("Erro I2C: %s\n", snap.last_i2c_error[0] ? snap.last_i2c_error : "nenhum");
    printf("Erro MQTT: %s\n",
           snap.last_mqtt_error[0] ? snap.last_mqtt_error : "nenhum");
}

void run_cli(CliArgs *args) {
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
