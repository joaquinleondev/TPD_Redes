/*
 * Servidor UDP Stop & Wait File Transfer Protocol - Versión con UI
 * Uso recomendado: compilar con el Makefile del proyecto.
 *    make server_ui
 *    ./server_ui <credentials_file>
 */

#include <arpa/inet.h>
#include <errno.h>
#include <ncurses.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "protocol.h"

#define MAX_CLIENTS 10
#define CLIENT_TIMEOUT 60
#define MAX_CREDENTIALS 100
#define MAX_LOG_ENTRIES 50

// Color pairs
#define COLOR_HEADER 1
#define COLOR_SUCCESS 2
#define COLOR_ERROR 3
#define COLOR_WARNING 4
#define COLOR_INFO 5
#define COLOR_DATA 6

// Estructura para mantener estado de cada cliente
typedef struct {
  struct sockaddr_in addr;
  ClientState state;
  uint8_t expected_seq;
  char filename[256];
  FILE *file;
  time_t last_activity;
  time_t start_time;
  int active;
  size_t bytes_received;
  uint8_t last_ack_seq;
  int has_last_ack;
} ClientSession;

// Entrada de log
typedef struct {
  time_t timestamp;
  char message[256];
  int color_pair;
} LogEntry;

// Lista de credenciales válidas
static char valid_credentials[MAX_CREDENTIALS][256];
static int num_credentials = 0;

// Pool de sesiones de clientes
static ClientSession clients[MAX_CLIENTS];

// Estadísticas globales
static struct {
  size_t total_bytes_received;
  int total_transfers_completed;
  int total_auth_attempts;
  int failed_auth_attempts;
  time_t server_start_time;
} stats = {0};

// Buffer de logs circular
static LogEntry log_buffer[MAX_LOG_ENTRIES];
static int log_head = 0;
static int log_count = 0;

// Windows de ncurses
static WINDOW *win_header = NULL;
static WINDOW *win_stats = NULL;
static WINDOW *win_clients = NULL;
static WINDOW *win_logs = NULL;

// Flag para actualización de UI
static volatile int ui_needs_update = 1;

// Log de servidor (archivo)
static FILE *server_log_file = NULL;

// Agregar entrada al log
static void add_log(const char *message, int color_pair) {
  time_t now = time(NULL);
  
  // Escribir en archivo
  if (server_log_file) {
    char time_str[64];
    struct tm *tm_info = localtime(&now);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
    fprintf(server_log_file, "[%s] %s\n", time_str, message);
    fflush(server_log_file);
  }

  log_buffer[log_head].timestamp = now;
  strncpy(log_buffer[log_head].message, message, 255);
  log_buffer[log_head].message[255] = '\0';
  log_buffer[log_head].color_pair = color_pair;

  log_head = (log_head + 1) % MAX_LOG_ENTRIES;
  if (log_count < MAX_LOG_ENTRIES) {
    log_count++;
  }

  ui_needs_update = 1;
}

// Obtener nombre del estado
static const char *get_state_name(ClientState state) {
  switch (state) {
  case STATE_IDLE:
    return "IDLE";
  case STATE_AUTHENTICATED:
    return "AUTH";
  case STATE_READY_TO_TRANSFER:
    return "READY";
  case STATE_TRANSFERRING:
    return "XFER";
  case STATE_COMPLETED:
    return "DONE";
  default:
    return "???";
  }
}

// Obtener color del estado
static int get_state_color(ClientState state) {
  switch (state) {
  case STATE_IDLE:
    return COLOR_INFO;
  case STATE_AUTHENTICATED:
    return COLOR_WARNING;
  case STATE_READY_TO_TRANSFER:
    return COLOR_WARNING;
  case STATE_TRANSFERRING:
    return COLOR_DATA;
  case STATE_COMPLETED:
    return COLOR_SUCCESS;
  default:
    return COLOR_INFO;
  }
}

// Formatear tiempo transcurrido
static void format_elapsed_time(time_t seconds, char *buf, size_t bufsize) {
  if (seconds < 60) {
    snprintf(buf, bufsize, "%lds", seconds);
  } else if (seconds < 3600) {
    snprintf(buf, bufsize, "%ldm%lds", seconds / 60, seconds % 60);
  } else {
    snprintf(buf, bufsize, "%ldh%ldm", seconds / 3600, (seconds % 3600) / 60);
  }
}

// Formatear tamaño en bytes
static void format_bytes(size_t bytes, char *buf, size_t bufsize) {
  if (bytes < 1024) {
    snprintf(buf, bufsize, "%zuB", bytes);
  } else if (bytes < 1024 * 1024) {
    snprintf(buf, bufsize, "%.1fKB", bytes / 1024.0);
  } else {
    snprintf(buf, bufsize, "%.2fMB", bytes / (1024.0 * 1024.0));
  }
}

// Inicializar ncurses
static void init_ui(void) {
  initscr();
  cbreak();
  noecho();
  curs_set(0);
  keypad(stdscr, TRUE);
  timeout(100); // 100ms timeout para getch()

  // Inicializar colores
  if (has_colors()) {
    start_color();
    init_pair(COLOR_HEADER, COLOR_BLACK, COLOR_CYAN);
    init_pair(COLOR_SUCCESS, COLOR_GREEN, COLOR_BLACK);
    init_pair(COLOR_ERROR, COLOR_RED, COLOR_BLACK);
    init_pair(COLOR_WARNING, COLOR_YELLOW, COLOR_BLACK);
    init_pair(COLOR_INFO, COLOR_CYAN, COLOR_BLACK);
    init_pair(COLOR_DATA, COLOR_MAGENTA, COLOR_BLACK);
  }

  // Crear ventanas
  int max_y, max_x;
  getmaxyx(stdscr, max_y, max_x);

  win_header = newwin(3, max_x, 0, 0);
  win_stats = newwin(5, max_x, 3, 0);
  win_clients = newwin(max_y - 8 - 12, max_x, 8, 0);
  win_logs = newwin(12, max_x, max_y - 12, 0);

  refresh();
}

// Actualizar UI
static void update_ui(void) {
  if (!ui_needs_update)
    return;

  int max_y, max_x;
  getmaxyx(stdscr, max_y, max_x);

  // === HEADER ===
  werase(win_header);
  wattron(win_header, COLOR_PAIR(COLOR_HEADER) | A_BOLD);
  mvwprintw(win_header, 0, 0, "%*s", max_x, "");
  mvwprintw(win_header, 1, (max_x - 40) / 2,
            "  SERVIDOR UDP STOP&WAIT - TRANSFER  ");
  mvwprintw(win_header, 2, 0, "%*s", max_x, "");
  wattroff(win_header, COLOR_PAIR(COLOR_HEADER) | A_BOLD);
  wrefresh(win_header);

  // === ESTADÍSTICAS ===
  werase(win_stats);
  box(win_stats, 0, 0);
  wattron(win_stats, A_BOLD);
  mvwprintw(win_stats, 0, 2, " ESTADÍSTICAS ");
  wattroff(win_stats, A_BOLD);

  time_t now = time(NULL);
  char uptime_str[32];
  format_elapsed_time(now - stats.server_start_time, uptime_str,
                      sizeof(uptime_str));

  char total_bytes_str[32];
  format_bytes(stats.total_bytes_received, total_bytes_str,
               sizeof(total_bytes_str));

  int active_count = 0;
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clients[i].active)
      active_count++;
  }

  mvwprintw(win_stats, 1, 2, "Uptime: ");
  wattron(win_stats, COLOR_PAIR(COLOR_INFO) | A_BOLD);
  wprintw(win_stats, "%s", uptime_str);
  wattroff(win_stats, COLOR_PAIR(COLOR_INFO) | A_BOLD);

  wprintw(win_stats, "  |  Clientes activos: ");
  wattron(win_stats,
          COLOR_PAIR(active_count > 0 ? COLOR_SUCCESS : COLOR_INFO) | A_BOLD);
  wprintw(win_stats, "%d/%d", active_count, MAX_CLIENTS);
  wattroff(win_stats,
           COLOR_PAIR(active_count > 0 ? COLOR_SUCCESS : COLOR_INFO) | A_BOLD);

  mvwprintw(win_stats, 2, 2, "Transferencias completadas: ");
  wattron(win_stats, COLOR_PAIR(COLOR_SUCCESS) | A_BOLD);
  wprintw(win_stats, "%d", stats.total_transfers_completed);
  wattroff(win_stats, COLOR_PAIR(COLOR_SUCCESS) | A_BOLD);

  wprintw(win_stats, "  |  Total recibido: ");
  wattron(win_stats, COLOR_PAIR(COLOR_DATA) | A_BOLD);
  wprintw(win_stats, "%s", total_bytes_str);
  wattroff(win_stats, COLOR_PAIR(COLOR_DATA) | A_BOLD);

  mvwprintw(win_stats, 3, 2, "Autenticaciones: ");
  wattron(win_stats, COLOR_PAIR(COLOR_SUCCESS));
  wprintw(win_stats, "%d OK",
          stats.total_auth_attempts - stats.failed_auth_attempts);
  wattroff(win_stats, COLOR_PAIR(COLOR_SUCCESS));
  wprintw(win_stats, " / ");
  wattron(win_stats, COLOR_PAIR(COLOR_ERROR));
  wprintw(win_stats, "%d ERR", stats.failed_auth_attempts);
  wattroff(win_stats, COLOR_PAIR(COLOR_ERROR));

  wrefresh(win_stats);

  // === CLIENTES ACTIVOS ===
  werase(win_clients);
  box(win_clients, 0, 0);
  wattron(win_clients, A_BOLD);
  mvwprintw(win_clients, 0, 2, " CLIENTES ACTIVOS ");
  wattroff(win_clients, A_BOLD);

  // Header de tabla
  wattron(win_clients, A_BOLD | A_UNDERLINE);
  mvwprintw(win_clients, 1, 2, "%-15s %-6s %-8s %-20s %-12s %-8s", "IP",
            "PUERTO", "ESTADO", "ARCHIVO", "BYTES", "TIEMPO");
  wattroff(win_clients, A_BOLD | A_UNDERLINE);

  int row = 3;
  for (int i = 0; i < MAX_CLIENTS && row < max_y - 8 - 12 - 2; i++) {
    if (!clients[i].active)
      continue;

    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &clients[i].addr.sin_addr, ip, sizeof(ip));

    char elapsed_str[16];
    format_elapsed_time(now - clients[i].start_time, elapsed_str,
                        sizeof(elapsed_str));

    char bytes_str[16];
    format_bytes(clients[i].bytes_received, bytes_str, sizeof(bytes_str));

    mvwprintw(win_clients, row, 2, "%-15s", ip);
    wprintw(win_clients, " %-6d", ntohs(clients[i].addr.sin_port));

    int state_color = get_state_color(clients[i].state);
    wattron(win_clients, COLOR_PAIR(state_color) | A_BOLD);
    wprintw(win_clients, " %-8s", get_state_name(clients[i].state));
    wattroff(win_clients, COLOR_PAIR(state_color) | A_BOLD);

    if (strlen(clients[i].filename) > 0) {
      wprintw(win_clients, " %-20s", clients[i].filename);
    } else {
      wprintw(win_clients, " %-20s", "-");
    }

    wprintw(win_clients, " %-12s", bytes_str);
    wprintw(win_clients, " %-8s", elapsed_str);

    row++;
  }

  if (active_count == 0) {
    wattron(win_clients, COLOR_PAIR(COLOR_INFO) | A_DIM);
    mvwprintw(win_clients, 3, (max_x - 30) / 2, "No hay clientes conectados");
    wattroff(win_clients, COLOR_PAIR(COLOR_INFO) | A_DIM);
  }

  wrefresh(win_clients);

  // === LOGS ===
  werase(win_logs);
  box(win_logs, 0, 0);
  wattron(win_logs, A_BOLD);
  mvwprintw(win_logs, 0, 2, " EVENTOS RECIENTES ");
  wattroff(win_logs, A_BOLD);

  int log_row = 10;
  int start_idx = (log_head - 1 + MAX_LOG_ENTRIES) % MAX_LOG_ENTRIES;
  int shown = 0;

  for (int i = 0; i < log_count && shown < 10; i++) {
    int idx = (start_idx - i + MAX_LOG_ENTRIES) % MAX_LOG_ENTRIES;
    LogEntry *entry = &log_buffer[idx];

    struct tm *tm_info = localtime(&entry->timestamp);
    char time_str[10];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);

    mvwprintw(win_logs, log_row, 2, "[%s]", time_str);
    wattron(win_logs, COLOR_PAIR(entry->color_pair));
    wprintw(win_logs, " %s", entry->message);
    wattroff(win_logs, COLOR_PAIR(entry->color_pair));

    log_row--;
    shown++;
  }

  wrefresh(win_logs);

  ui_needs_update = 0;
}

// Limpiar UI
static void cleanup_ui(void) {
  if (win_header)
    delwin(win_header);
  if (win_stats)
    delwin(win_stats);
  if (win_clients)
    delwin(win_clients);
  if (win_logs)
    delwin(win_logs);
  endwin();
}

// Cargar credenciales desde archivo
static int load_credentials(const char *filename) {
  FILE *f = fopen(filename, "r");
  if (!f) {
    perror("fopen credentials");
    return -1;
  }

  num_credentials = 0;
  while (num_credentials < MAX_CREDENTIALS &&
         fgets(valid_credentials[num_credentials], 256, f)) {
    size_t len = strlen(valid_credentials[num_credentials]);
    if (len > 0 && valid_credentials[num_credentials][len - 1] == '\n') {
      valid_credentials[num_credentials][len - 1] = '\0';
    }
    num_credentials++;
  }

  fclose(f);
  char log_msg[256];
  snprintf(log_msg, sizeof(log_msg), "Cargadas %d credenciales del archivo",
           num_credentials);
  add_log(log_msg, COLOR_SUCCESS);
  return 0;
}

// Verificar si una credencial es válida
static int is_valid_credential(const char *cred) {
  for (int i = 0; i < num_credentials; i++) {
    if (strcmp(valid_credentials[i], cred) == 0) {
      return 1;
    }
  }
  return 0;
}

// Comparar direcciones de socket
static int addr_equal(struct sockaddr_in *a, struct sockaddr_in *b) {
  return (a->sin_addr.s_addr == b->sin_addr.s_addr &&
          a->sin_port == b->sin_port);
}

// Encontrar o crear sesión de cliente
static ClientSession *find_or_create_session(struct sockaddr_in *addr) {
  time_t now = time(NULL);
  ClientSession *free_slot = NULL;

  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clients[i].active && addr_equal(&clients[i].addr, addr)) {
      clients[i].last_activity = now;
      return &clients[i];
    }
    if (!clients[i].active && !free_slot) {
      free_slot = &clients[i];
    }
  }

  if (free_slot) {
    memset(free_slot, 0, sizeof(ClientSession));
    free_slot->addr = *addr;
    free_slot->state = STATE_IDLE;
    free_slot->expected_seq = 0;
    free_slot->last_activity = now;
    free_slot->start_time = now;
    free_slot->active = 1;
    free_slot->file = NULL;

    char log_msg[256];
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
    snprintf(log_msg, sizeof(log_msg), "Nueva conexión desde %s:%d", ip,
             ntohs(addr->sin_port));
    add_log(log_msg, COLOR_INFO);

    return free_slot;
  }

  return NULL;
}

// Liberar recursos de una sesión
static void cleanup_session(ClientSession *session) {
  if (session->file) {
    fclose(session->file);
    session->file = NULL;
  }

  char log_msg[256];
  char ip[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &session->addr.sin_addr, ip, sizeof(ip));

  if (session->state == STATE_COMPLETED) {
    char bytes_str[32];
    format_bytes(session->bytes_received, bytes_str, sizeof(bytes_str));
    snprintf(log_msg, sizeof(log_msg), "✓ Transferencia completada: %s (%s)",
             session->filename, bytes_str);
    add_log(log_msg, COLOR_SUCCESS);
    stats.total_transfers_completed++;
  } else {
    snprintf(log_msg, sizeof(log_msg), "Sesión cerrada: %s:%d", ip,
             ntohs(session->addr.sin_port));
    add_log(log_msg, COLOR_WARNING);
  }

  session->active = 0;
}

// Limpiar sesiones inactivas
static void cleanup_inactive_sessions(void) {
  time_t now = time(NULL);

  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clients[i].active &&
        (now - clients[i].last_activity) > CLIENT_TIMEOUT) {
      add_log("Timeout de sesión por inactividad", COLOR_WARNING);
      cleanup_session(&clients[i]);
    }
  }
}

// Enviar ACK
static void send_ack(int sockfd, struct sockaddr_in *addr, uint8_t seq_num,
                     const char *error_msg) {
  uint8_t buffer[MAX_PDU_SIZE];
  size_t pdu_size = 2;

  buffer[0] = TYPE_ACK;
  buffer[1] = seq_num;

  if (error_msg) {
    size_t msg_len = strlen(error_msg);
    if (msg_len > MAX_DATA_SIZE) {
      msg_len = MAX_DATA_SIZE;
    }
    memcpy(buffer + 2, error_msg, msg_len);
    pdu_size += msg_len;
  }

  sendto(sockfd, buffer, pdu_size, 0, (struct sockaddr *)addr, sizeof(*addr));
}

// Manejar PDU HELLO
static void handle_hello(int sockfd, struct sockaddr_in *addr, uint8_t *data,
                         size_t data_len, uint8_t seq_num) {
  ClientSession *session = find_or_create_session(addr);
  if (!session) {
    add_log("Sin espacio para nuevos clientes", COLOR_ERROR);
    return;
  }

  if (seq_num != 0) {
    add_log("HELLO con Seq != 0, descartado", COLOR_WARNING);
    return;
  }

  char credentials[256];
  size_t cred_len = (data_len < 255) ? data_len : 255;
  memcpy(credentials, data, cred_len);
  credentials[cred_len] = '\0';

  stats.total_auth_attempts++;

  if (session->state == STATE_IDLE) {
    if (!is_valid_credential(credentials)) {
      char log_msg[256];
      char ip[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &session->addr.sin_addr, ip, sizeof(ip));
      snprintf(log_msg, sizeof(log_msg), "✗ Autenticación fallida: %s", ip);
      add_log(log_msg, COLOR_ERROR);
      stats.failed_auth_attempts++;
      send_ack(sockfd, addr, 0, "Invalid credentials");
      cleanup_session(session);
      return;
    }

    session->state = STATE_AUTHENTICATED;
    session->expected_seq = 1;
    session->last_ack_seq = 0;
    session->has_last_ack = 1;
    send_ack(sockfd, addr, 0, NULL);

    char log_msg[256];
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &session->addr.sin_addr, ip, sizeof(ip));
    snprintf(log_msg, sizeof(log_msg), "✓ Cliente autenticado: %s", ip);
    add_log(log_msg, COLOR_SUCCESS);
  } else {
    if (session->has_last_ack && session->last_ack_seq == 0) {
      send_ack(sockfd, addr, 0, NULL);
    }
  }
}

// Manejar PDU WRQ
static void handle_wrq(int sockfd, struct sockaddr_in *addr, uint8_t *data,
                       size_t data_len, uint8_t seq_num) {
  ClientSession *session = find_or_create_session(addr);
  if (!session)
    return;

  if (seq_num != 1) {
    add_log("WRQ con Seq != 1, descartado", COLOR_WARNING);
    return;
  }

  char filename[256];
  size_t fn_len = 0;
  for (size_t i = 0; i < data_len && i < 255; i++) {
    if (data[i] == '\0') {
      fn_len = i;
      break;
    }
    filename[i] = (char)data[i];
  }
  filename[fn_len] = '\0';

  if (session->state == STATE_AUTHENTICATED) {
    if (fn_len < 4 || fn_len > 10) {
      add_log("WRQ rechazado: longitud de nombre inválida", COLOR_ERROR);
      send_ack(sockfd, addr, 1, "Filename length must be 4-10 chars");
      return;
    }

    for (size_t i = 0; i < fn_len; i++) {
      unsigned char c = (unsigned char)filename[i];
      if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') || c == '_' || c == '-' || c == '.')) {
        add_log("WRQ rechazado: caracteres inválidos en nombre", COLOR_ERROR);
        send_ack(sockfd, addr, 1, "Invalid filename characters");
        return;
      }
    }

    if (mkdir("uploads", 0755) < 0 && errno != EEXIST) {
      add_log("Error creando directorio uploads", COLOR_ERROR);
      send_ack(sockfd, addr, 1, "Server error");
      return;
    }

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "uploads/%s", filename);
    session->file = fopen(filepath, "wb");

    if (!session->file) {
      add_log("Error creando archivo", COLOR_ERROR);
      send_ack(sockfd, addr, 1, "Cannot create file");
      return;
    }

    strcpy(session->filename, filename);
    session->state = STATE_READY_TO_TRANSFER;
    session->expected_seq = 0;
    session->has_last_ack = 1;
    session->last_ack_seq = 1;
    send_ack(sockfd, addr, 1, NULL);

    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "Iniciando transferencia: %s", filename);
    add_log(log_msg, COLOR_INFO);
  } else if (session->state == STATE_READY_TO_TRANSFER ||
             session->state == STATE_TRANSFERRING) {
    if (strcmp(session->filename, filename) == 0) {
      send_ack(sockfd, addr, 1, NULL);
    } else {
      send_ack(sockfd, addr, 1, "Filename mismatch");
    }
  }
}

// Manejar PDU DATA
static void handle_data(int sockfd, struct sockaddr_in *addr, uint8_t *data,
                        size_t data_len, uint8_t seq_num) {
  ClientSession *session = find_or_create_session(addr);
  if (!session)
    return;

  if (session->state != STATE_READY_TO_TRANSFER &&
      session->state != STATE_TRANSFERRING) {
    add_log("DATA sin WRQ previo, descartado", COLOR_WARNING);
    return;
  }

  if (seq_num == session->expected_seq) {
    if (data_len > 0) {
      size_t written = fwrite(data, 1, data_len, session->file);
      if (written != data_len) {
        add_log("Error escribiendo archivo", COLOR_ERROR);
        cleanup_session(session);
        return;
      }
      session->bytes_received += written;
      stats.total_bytes_received += written;
    }

    send_ack(sockfd, addr, seq_num, NULL);
    session->state = STATE_TRANSFERRING;
    session->expected_seq = 1 - seq_num;
    session->last_ack_seq = seq_num;
    session->has_last_ack = 1;
    ui_needs_update = 1;
  } else {
    if (session->has_last_ack && seq_num == session->last_ack_seq) {
      send_ack(sockfd, addr, session->last_ack_seq, NULL);
    }
  }
}

// Manejar PDU FIN
static void handle_fin(int sockfd, struct sockaddr_in *addr, uint8_t *data,
                       size_t data_len, uint8_t seq_num) {
  ClientSession *session = find_or_create_session(addr);
  if (!session)
    return;

  char filename[256];
  size_t fn_len = 0;
  for (size_t i = 0; i < data_len && i < 255; i++) {
    if (data[i] == '\0') {
      fn_len = i;
      break;
    }
    filename[i] = (char)data[i];
  }
  filename[fn_len] = '\0';

  if (session->state == STATE_TRANSFERRING) {
    if (seq_num != session->expected_seq) {
      add_log("FIN con Seq num incorrecto, descartado", COLOR_WARNING);
      return;
    }

    if (strcmp(filename, session->filename) != 0) {
      add_log("FIN con nombre de archivo incorrecto", COLOR_ERROR);
      send_ack(sockfd, addr, seq_num, "Filename mismatch");
      cleanup_session(session);
      return;
    }

    if (session->file) {
      fclose(session->file);
      session->file = NULL;
    }
    send_ack(sockfd, addr, seq_num, NULL);
    session->state = STATE_COMPLETED;
    session->last_ack_seq = seq_num;
    session->has_last_ack = 1;
    cleanup_session(session);
  } else if (session->state == STATE_COMPLETED) {
    if (strcmp(filename, session->filename) == 0 && session->has_last_ack &&
        seq_num == session->last_ack_seq) {
      send_ack(sockfd, addr, seq_num, NULL);
    }
  }
}

int main(int argc, char *argv[]) {
  if (argc != 2) {
    fprintf(stderr, "Uso: %s <credentials_file>\n", argv[0]);
    return 1;
  }

  printf("Máximo de clientes concurrentes: %d\n", MAX_CLIENTS);

  // Abrir log file
  server_log_file = fopen("server.log", "a");
  if (!server_log_file) {
    perror("fopen server.log");
  }

  // Inicializar estadísticas
  stats.server_start_time = time(NULL);

  // Inicializar UI
  init_ui();

  // Cargar credenciales
  if (load_credentials(argv[1]) < 0) {
    cleanup_ui();
    return 1;
  }

  // Inicializar pool de clientes
  memset(clients, 0, sizeof(clients));

  // Crear socket UDP
  int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0) {
    cleanup_ui();
    perror("socket");
    return 1;
  }

  // Configurar timeout para que no bloquee indefinidamente
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 100000; // 100ms
  setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  // Permitir reutilización de dirección
  int reuse = 1;
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
    cleanup_ui();
    perror("setsockopt");
    close(sockfd);
    return 1;
  }

  // Vincular a puerto
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(SERVER_PORT);

  if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    cleanup_ui();
    perror("bind");
    close(sockfd);
    return 1;
  }

  char log_msg[256];
  snprintf(log_msg, sizeof(log_msg), "Servidor iniciado en puerto %d",
           SERVER_PORT);
  add_log(log_msg, COLOR_SUCCESS);

  // Loop principal
  uint8_t buffer[MAX_PDU_SIZE];

  while (1) {
    // Actualizar UI
    update_ui();

    // Verificar tecla de salida (q)
    int ch = getch();
    if (ch == 'q' || ch == 'Q') {
      break;
    }

    // Limpiar sesiones inactivas
    cleanup_inactive_sessions();

    // Recibir PDU
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    ssize_t recv_len = recvfrom(sockfd, buffer, MAX_PDU_SIZE, 0,
                                (struct sockaddr *)&client_addr, &client_len);

    if (recv_len < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        continue; // Timeout, continuar
      }
      if (errno == EINTR) {
        continue;
      }
      break;
    }

    if (recv_len < 2) {
      continue;
    }

    // Extraer campos
    uint8_t type = buffer[0];
    uint8_t seq_num = buffer[1];
    uint8_t *data = (recv_len > 2) ? &buffer[2] : NULL;
    size_t data_len = (recv_len > 2) ? (size_t)(recv_len - 2) : 0;

    // Procesar según tipo
    switch (type) {
    case TYPE_HELLO:
      handle_hello(sockfd, &client_addr, data, data_len, seq_num);
      break;
    case TYPE_WRQ:
      handle_wrq(sockfd, &client_addr, data, data_len, seq_num);
      break;
    case TYPE_DATA:
      handle_data(sockfd, &client_addr, data, data_len, seq_num);
      break;
    case TYPE_FIN:
      handle_fin(sockfd, &client_addr, data, data_len, seq_num);
      break;
    default:
      break;
    }
  }

  // Cleanup
  add_log("Cerrando servidor...", COLOR_WARNING);
  update_ui();
  sleep(1); // Esperar un momento para mostrar el mensaje

  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clients[i].active) {
      cleanup_session(&clients[i]);
    }
  }

  close(sockfd);
  cleanup_ui();

  if (server_log_file) {
    fclose(server_log_file);
  }

  printf("\n¡Servidor cerrado!\n");
  return 0;
}


