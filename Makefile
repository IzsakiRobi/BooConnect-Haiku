CXX ?= g++
CXXFLAGS ?= -Wall -Wextra -std=c++17 -O2
LDLIBS ?= -lbe -ltracker -ltranslation -lroot

TARGET := BooConnect
SOURCES := src/main.cpp

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CXX) $(CXXFLAGS) -o $@ $(SOURCES) $(LDLIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
