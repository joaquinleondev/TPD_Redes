CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c99 -D_POSIX_C_SOURCE=200809L -Iinclude
LDFLAGS =
LDFLAGS_NCURSES = -lncurses

SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin
INCLUDE_DIR = include

# Common objects
COMMON_SRCS = $(SRC_DIR)/common/common.c
COMMON_OBJS = $(OBJ_DIR)/common/common.o

# UDP Targets
UDP_CLIENT_SRC = $(SRC_DIR)/udp/client.c
UDP_SERVER_SRC = $(SRC_DIR)/udp/server.c
UDP_CLIENT_UI_SRC = $(SRC_DIR)/udp/client_ui.c
UDP_SERVER_UI_SRC = $(SRC_DIR)/udp/server_ui.c

UDP_CLIENT_OBJ = $(OBJ_DIR)/udp/client.o
UDP_SERVER_OBJ = $(OBJ_DIR)/udp/server.o
UDP_CLIENT_UI_OBJ = $(OBJ_DIR)/udp/client_ui.o
UDP_SERVER_UI_OBJ = $(OBJ_DIR)/udp/server_ui.o

# TCP Targets
TCP_CLIENT_SRC = $(SRC_DIR)/tcp/client.c
TCP_SERVER_SRC = $(SRC_DIR)/tcp/server.c

TCP_CLIENT_OBJ = $(OBJ_DIR)/tcp/client.o
TCP_SERVER_OBJ = $(OBJ_DIR)/tcp/server.o

# All binaries
TARGETS = $(BIN_DIR)/udp_client $(BIN_DIR)/udp_server \
          $(BIN_DIR)/udp_client_ui $(BIN_DIR)/udp_server_ui \
          $(BIN_DIR)/tcp_client $(BIN_DIR)/tcp_server

.PHONY: all clean udp tcp test directories

all: directories $(TARGETS)

udp: directories $(BIN_DIR)/udp_client $(BIN_DIR)/udp_server $(BIN_DIR)/udp_client_ui $(BIN_DIR)/udp_server_ui

tcp: directories $(BIN_DIR)/tcp_client $(BIN_DIR)/tcp_server

directories:
	@mkdir -p $(BIN_DIR)
	@mkdir -p $(OBJ_DIR)/common
	@mkdir -p $(OBJ_DIR)/udp
	@mkdir -p $(OBJ_DIR)/tcp

# Common
$(OBJ_DIR)/common/common.o: $(SRC_DIR)/common/common.c $(INCLUDE_DIR)/common.h
	$(CC) $(CFLAGS) -c $< -o $@

# UDP Rules
$(OBJ_DIR)/udp/%.o: $(SRC_DIR)/udp/%.c $(SRC_DIR)/udp/protocol.h
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR)/udp_client: $(UDP_CLIENT_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BIN_DIR)/udp_server: $(UDP_SERVER_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BIN_DIR)/udp_client_ui: $(UDP_CLIENT_UI_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS_NCURSES)

$(BIN_DIR)/udp_server_ui: $(UDP_SERVER_UI_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS_NCURSES)

# TCP Rules
$(OBJ_DIR)/tcp/%.o: $(SRC_DIR)/tcp/%.c $(INCLUDE_DIR)/common.h
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR)/tcp_client: $(TCP_CLIENT_OBJ) $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BIN_DIR)/tcp_server: $(TCP_SERVER_OBJ) $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)
	rm -rf uploads/
	rm -f *.log *.csv

test: all
	@echo "Running UDP tests..."
	./tests/test_udp.sh
	@echo "Running TCP tests..."
	./tests/test_tcp.sh
