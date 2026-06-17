CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2
LDLIBS ?= -lcurl -pthread

TARGET := temperature_bot
SOURCE := temperature_bot.cpp

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CXX) $(CXXFLAGS) $(SOURCE) $(LDLIBS) -o $(TARGET)

clean:
	rm -f $(TARGET)
