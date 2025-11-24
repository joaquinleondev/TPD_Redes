CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c99 -D_POSIX_C_SOURCE=200809L

BIN_DIR = bin

.PHONY: all udp tcp clean

all: udp tcp

udp: $(BIN_DIR)/udp_client $(BIN_DIR)/udp_server

tcp: $(BIN_DIR)/tcp_client $(BIN_DIR)/tcp_server

$(BIN_DIR):
	@mkdir -p $(BIN_DIR)

# UDP
$(BIN_DIR)/udp_client: src/udp/client.c src/udp/protocol.h | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ src/udp/client.c

$(BIN_DIR)/udp_server: src/udp/server.c src/udp/protocol.h | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ src/udp/server.c

# TCP
$(BIN_DIR)/tcp_client: src/tcp/client.c src/tcp/common.c src/tcp/common.h | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ src/tcp/client.c src/tcp/common.c

$(BIN_DIR)/tcp_server: src/tcp/server.c src/tcp/common.c src/tcp/common.h | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ src/tcp/server.c src/tcp/common.c

clean:
	rm -rf $(BIN_DIR)
