# Makefile for atomic_fetch_add
CC       = cc
CFLAGS   = -std=c11 -O2 -Wall -Wextra
LDFLAGS  = -static -lpthread

TARGET   = atomic_fetch_add
SRC      = atomic_fetch_add.c

.PHONY: all clean run help

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

run: $(TARGET)
	@echo "Usage example: ./$(TARGET) 4 0x100000000 1000000"
	@echo "WARNING: Do not run on arbitrary addresses – use a valid shared-memory address."

clean:
	rm -f $(TARGET)

help:
	@echo "Usage:"
	@echo "  make          – build $(TARGET)"
	@echo "  make run      – show usage hint"
	@echo "  make clean    – remove binary"