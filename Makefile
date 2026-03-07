CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -Iinclude -O2
LDFLAGS = -lffi -ldl

SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:.c=.o)
TARGET = viper
TIMEOUT ?= 10

EXT_NET = lib/std/net.so
EXT_MATH = lib/std/math.so

.PHONY: all clean test

all: $(TARGET) $(EXT_NET) $(EXT_MATH)

$(EXT_NET): lib/std/net_ext.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $<

$(EXT_MATH): lib/std/math_ext.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $< -lm

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test: $(TARGET)
	@./tests/run_tests.sh ./$(TARGET) tests/scripts $(TIMEOUT)

clean:
	rm -f $(OBJS) $(TARGET) $(EXT_NET) $(EXT_MATH)
