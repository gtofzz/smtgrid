# Makefile para mqtt_server_sim
# Uso:
#   make        -> compila mqtt_server_sim
#   make clean  -> remove binário

CC      := gcc
CFLAGS  := -Wall -Wextra -O2
LDFLAGS := -lmosquitto -lpthread

SRC     := mqtt_server_sim.c
BIN     := mqtt_server_sim

.PHONY: all clean

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(BIN)

