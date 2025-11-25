CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -pedantic -O2
LDFLAGS :=
TARGET := mqtt_debug_server
SRC := main.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) $(LDFLAGS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all clean run
