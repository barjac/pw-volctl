CC      = gcc
TARGET  = pw-volctl
SRC     = pw-volctl11.c

CFLAGS  = $(shell pkg-config --cflags gtk4) -Wall -Wextra -O2
LIBS    = $(shell pkg-config --libs gtk4) -lm

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

install: $(TARGET)
	install -Dm755 $(TARGET) $(HOME)/.local/bin/$(TARGET)

clean:
	rm -f $(TARGET)
