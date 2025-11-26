# Makefile para rasp_node
# Alvos:
#   make sim   -> compila com I2C SIMULADO (thread "fake STM" + memória compartilhada)
#   make real  -> compila para I2C REAL (/dev/i2c-1), uso no Raspberry
#   make clean -> remove binários

CC      := gcc
CFLAGS  := -Wall -Wextra -O2
LDFLAGS := -lmosquitto -lpthread

SRC     := rasp_node.c

BIN_SIM  := rasp_node_sim
BIN_REAL := rasp_node_real

.PHONY: all sim real clean

all: sim

sim: $(BIN_SIM)

real: $(BIN_REAL)

$(BIN_SIM): $(SRC)
	$(CC) $(CFLAGS) -DSIM_I2C -o $@ $^ $(LDFLAGS)

$(BIN_REAL): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(BIN_SIM) $(BIN_REAL)

