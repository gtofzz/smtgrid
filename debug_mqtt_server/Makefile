# Makefile para mqtt_server_sim
# Uso:
#   make        -> compila mqtt_server_sim
#   make clean  -> remove binário

CONFIG_FILE ?= $(abspath ../config)
-include $(CONFIG_FILE)

CC ?= gcc
COMMON_CFLAGS ?= -Wall -Wextra -O2
COMMON_LDFLAGS ?= -lmosquitto -lpthread
MQTT_HOST ?= localhost
MQTT_PORT ?= 1883
MQTT_KEEPALIVE ?= 60

CFLAGS ?= $(COMMON_CFLAGS)
LDFLAGS ?= $(COMMON_LDFLAGS)
MQTT_DEFINES := -DMQTT_HOST_DEFAULT="$(MQTT_HOST)" -DMQTT_PORT_DEFAULT=$(MQTT_PORT) -DMQTT_KEEPALIVE=$(MQTT_KEEPALIVE)

SRC     := mqtt_server_sim.c
BIN     := mqtt_server_sim

.PHONY: all clean

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(MQTT_DEFINES) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(BIN)
