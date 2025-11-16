CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c99 -D_POSIX_C_SOURCE=200809L
LDFLAGS = 
LDFLAGS_NCURSES = -lncurses

all: client server client_ui server_ui

# Versiones originales
client: client.c
	$(CC) $(CFLAGS) -o client client.c $(LDFLAGS)

server: server.c
	$(CC) $(CFLAGS) -o server server.c $(LDFLAGS)

# Versiones con UI mejorada
client_ui: client_ui.c
	$(CC) $(CFLAGS) -o client_ui client_ui.c $(LDFLAGS)

server_ui: server_ui.c
	$(CC) $(CFLAGS) -o server_ui server_ui.c $(LDFLAGS_NCURSES)

clean:
	rm -f client server client_ui server_ui
	rm -rf uploads/

debug: CFLAGS += -g -DDEBUG
debug: all

test: all
	./test.sh

.PHONY: all clean debug test