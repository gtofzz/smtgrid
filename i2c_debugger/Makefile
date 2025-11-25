CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -pedantic -O2
TARGET := i2c_debugger
SRC := main.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all clean run
