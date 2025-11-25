CC ?= gcc
CFLAGS ?= -Wall -Wextra -std=c11 -pthread
LDFLAGS ?= -lmosquitto -pthread

TARGET ?= no_mqtt_i2c
SRC := app.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)
