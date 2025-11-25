CC ?= gcc
CFLAGS ?= -Wall -Wextra -std=c11 -pthread
LDFLAGS ?= -lmosquitto -pthread

SRC := main.c config.c state.c mqtt_client.c i2c_link.c publisher.c cli.c
OBJ := $(SRC:.c=.o)
TARGET := no_mqtt_i2c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)
