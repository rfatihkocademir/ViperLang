CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -Iinclude -O2

SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:.c=.o)
TARGET = viper

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(OBJS) $(TARGET)
