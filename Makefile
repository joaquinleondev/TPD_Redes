CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c99 -D_POSIX_C_SOURCE=200809L
LDFLAGS =
LDFLAGS_NCURSES = -lncurses

# ----------------------------------------------------------------------
# Targets principales
# ----------------------------------------------------------------------
# Incluyo también tcp_client y tcp_server en 'all' para tener TODO el TP
# construido con un solo 'make all'.
all: client server client_ui server_ui tcp_client tcp_server

# ----------------------------------------------------------------------
# Parte 1 - UDP Stop & Wait
# ----------------------------------------------------------------------
# Versiones originales
client: client.c
	$(CC) $(CFLAGS) -o client client.c $(LDFLAGS)

server: server.c
	$(CC) $(CFLAGS) -o server server.c $(LDFLAGS)

# Versiones con UI mejorada (ncurses)
client_ui: client_ui.c
	$(CC) $(CFLAGS) -o client_ui client_ui.c $(LDFLAGS_NCURSES)

server_ui: server_ui.c
	$(CC) $(CFLAGS) -o server_ui server_ui.c $(LDFLAGS_NCURSES)

# ----------------------------------------------------------------------
# Parte 2 - TCP one-way delay
# ----------------------------------------------------------------------
tcp_client: tcp_client.c
	$(CC) $(CFLAGS) -o tcp_client tcp_client.c $(LDFLAGS)

tcp_server: tcp_server.c
	$(CC) $(CFLAGS) -o tcp_server tcp_server.c $(LDFLAGS)

# ----------------------------------------------------------------------
# Utilidades
# ----------------------------------------------------------------------
clean:
	rm -f client server client_ui server_ui tcp_client tcp_server
	rm -rf uploads/

debug: CFLAGS += -g -DDEBUG
debug: all

test: all
	./test.sh

.PHONY: all clean debug test tcp_client tcp_server
