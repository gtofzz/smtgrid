# Makefile para rasp_node
# Alvos:
#   make        -> compila modos sim e real
#   make sim    -> compila com I2C SIMULADO (thread "fake STM" + memória compartilhada)
#   make real   -> compila para I2C REAL (/dev/i2c-1), uso no Raspberry
#   make clean  -> remove binários

CONFIG_FILE ?= $(abspath ../config)
-include $(CONFIG_FILE)

CC ?= gcc
COMMON_CFLAGS ?= -Wall -Wextra -O2
COMMON_LDFLAGS ?= -lmosquitto -lpthread
MQTT_HOST ?= localhost
MQTT_PORT ?= 1883
MQTT_KEEPALIVE ?= 60
I2C_DEVICE ?= /dev/i2c-1
I2C_STM_ADDRESS ?= 0x20

CFLAGS ?= $(COMMON_CFLAGS)
LDFLAGS ?= $(COMMON_LDFLAGS)
MQTT_DEFINES := -DMQTT_HOST_DEFAULT="$(MQTT_HOST)" -DMQTT_PORT_DEFAULT=$(MQTT_PORT) -DMQTT_KEEPALIVE=$(MQTT_KEEPALIVE)
I2C_DEFINES := -DI2C_DEVICE_DEFAULT="$(I2C_DEVICE)" -DI2C_STM_ADDRESS=$(I2C_STM_ADDRESS)

SRC     := rasp_node.c

BIN_SIM  := rasp_node_sim
BIN_REAL := rasp_node_real

.PHONY: all sim real clean

all: sim real

sim: $(BIN_SIM)

real: $(BIN_REAL)

$(BIN_SIM): $(SRC)
	$(CC) $(CFLAGS) $(MQTT_DEFINES) -DSIM_I2C -o $@ $^ $(LDFLAGS)

$(BIN_REAL): $(SRC)
	$(CC) $(CFLAGS) $(MQTT_DEFINES) $(I2C_DEFINES) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(BIN_SIM) $(BIN_REAL)
