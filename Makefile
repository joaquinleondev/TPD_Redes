CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c99 -D_POSIX_C_SOURCE=200809L
LDFLAGS = 

all: client server

client: client.c
	$(CC) $(CFLAGS) -o client client.c $(LDFLAGS)

server: server.c
	$(CC) $(CFLAGS) -o server server.c $(LDFLAGS)

clean:
	rm -f client server
	rm -rf uploads/

debug: CFLAGS += -g -DDEBUG
debug: all

test: all
	./test.sh

.PHONY: all clean debug test