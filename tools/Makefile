# Build tools for local testing

CXX = clang++
CXXFLAGS = -std=c++17 -O2 -I../src/dsp -Wall

SRCS = ../src/dsp/mcu.cpp ../src/dsp/mcu_opcodes.cpp ../src/dsp/pcm.cpp

all: find_perf_offset

find_perf_offset: find_perf_offset.cpp $(SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $^

clean:
	rm -f find_perf_offset sram_dump.bin

.PHONY: all clean
