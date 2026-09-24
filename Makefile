CC      = gcc
TARGET  = pw-volctl
SRC     = pw-volctl.c

CFLAGS  = $(shell pkg-config --cflags gtk4 libpulse libpulse-mainloop-glib) -Wall -Wextra -O2
LIBS    = $(shell pkg-config --libs gtk4 libpulse libpulse-mainloop-glib) -lm

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

install: $(TARGET)
	install -Dm755 $(TARGET) $(HOME)/.local/bin/$(TARGET)

clean:
	rm -f $(TARGET)
