/*
 * Cliente UDP Stop & Wait File Transfer Protocol - Versión con UI
 * Uso recomendado: compilar con el Makefile del proyecto.
 *    make client_ui
 *    ./client_ui <server_ip> <filename> <credentials>
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "protocol.h"

// Colores ANSI
#define COLOR_RESET "\033[0m"
#define COLOR_BOLD "\033[1m"
#define COLOR_GREEN "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_BLUE "\033[34m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_CYAN "\033[36m"
#define COLOR_RED "\033[31m"
#define COLOR_DIM "\033[2m"

// Estadísticas de transferencia
static struct {
  size_t total_bytes;
  size_t bytes_sent;
  int packets_sent;
  int retransmissions;
  struct timeval start_time;
} transfer_stats = {0};

static int addr_equal(const struct sockaddr_in *a,
                      const struct sockaddr_in *b) {
  return a->sin_family == b->sin_family && a->sin_port == b->sin_port &&
         a->sin_addr.s_addr == b->sin_addr.s_addr;
}

// Obtener tamaño de archivo
static size_t get_file_size(FILE *file) {
  struct stat st;
  if (fstat(fileno(file), &st) == 0) {
    return (size_t)st.st_size;
  }
  return 0;
}

// Formatear bytes
static void format_bytes(size_t bytes, char *buf, size_t bufsize) {
  if (bytes < 1024) {
    snprintf(buf, bufsize, "%zu B", bytes);
  } else if (bytes < 1024 * 1024) {
    snprintf(buf, bufsize, "%.2f KB", bytes / 1024.0);
  } else {
    snprintf(buf, bufsize, "%.2f MB", bytes / (1024.0 * 1024.0));
  }
}

// Calcular velocidad
static void format_speed(size_t bytes, double seconds, char *buf,
                         size_t bufsize) {
  if (seconds <= 0) {
    snprintf(buf, bufsize, "-- B/s");
    return;
  }

  double bps = bytes / seconds;

  if (bps < 1024) {
    snprintf(buf, bufsize, "%.0f B/s", bps);
  } else if (bps < 1024 * 1024) {
    snprintf(buf, bufsize, "%.2f KB/s", bps / 1024.0);
  } else {
    snprintf(buf, bufsize, "%.2f MB/s", bps / (1024.0 * 1024.0));
  }
}

// Mostrar barra de progreso
static void show_progress_bar(const char *label, size_t current,
                              size_t total) {
  int bar_width = 40;
  float progress = (total > 0) ? ((float)current / total) : 0;
  if (progress > 1.0f)
    progress = 1.0f;

  int filled = (int)(bar_width * progress);

  char bytes_current[32], bytes_total[32];
  format_bytes(current, bytes_current, sizeof(bytes_current));
  format_bytes(total, bytes_total, sizeof(bytes_total));

  // Calcular tiempo transcurrido y velocidad
  struct timeval now;
  gettimeofday(&now, NULL);
  double elapsed =
      (now.tv_sec - transfer_stats.start_time.tv_sec) +
      (now.tv_usec - transfer_stats.start_time.tv_usec) / 1000000.0;

  char speed_str[32];
  format_speed(current, elapsed, speed_str, sizeof(speed_str));

  // Limpiar línea
  printf("\r\033[K");

  // Mostrar label
  printf("%s%s%s ", COLOR_CYAN, label, COLOR_RESET);

  // Mostrar barra
  printf("[");
  for (int i = 0; i < bar_width; i++) {
    if (i < filled) {
      printf("%s█%s", COLOR_GREEN, COLOR_RESET);
    } else {
      printf("%s░%s", COLOR_DIM, COLOR_RESET);
    }
  }
  printf("]");

  // Mostrar porcentaje
  printf(" %s%3.0f%%%s", COLOR_BOLD, progress * 100, COLOR_RESET);

  // Mostrar bytes
  printf(" %s%s%s/%s", COLOR_YELLOW, bytes_current, COLOR_RESET, bytes_total);

  // Mostrar velocidad
  printf(" %s%s%s", COLOR_MAGENTA, speed_str, COLOR_RESET);

  fflush(stdout);
}

// Mostrar header
static void show_header(const char *server_ip, const char *filename) {
  printf("\n");
  printf(
      "%s╔══════════════════════════════════════════════════════════════╗%s\n",
      COLOR_CYAN, COLOR_RESET);
  printf(
      "%s║%s          CLIENTE UDP STOP&WAIT - FILE TRANSFER          %s║%s\n",
      COLOR_CYAN, COLOR_BOLD, COLOR_CYAN, COLOR_RESET);
  printf(
      "%s╚══════════════════════════════════════════════════════════════╝%s\n",
      COLOR_CYAN, COLOR_RESET);
  printf("\n");
  printf("  %sServidor:%s %s\n", COLOR_DIM, COLOR_RESET, server_ip);
  printf("  %sArchivo:%s  %s\n", COLOR_DIM, COLOR_RESET, filename);
  printf("\n");
}

// Mostrar estado de fase
static void show_phase_status(const char *phase_name, const char *status,
                              int is_success) {
  const char *color = is_success ? COLOR_GREEN : COLOR_YELLOW;
  const char *icon = is_success ? "✓" : "→";

  printf("  %s[%s]%s %s%s%s: %s\n", color, icon, COLOR_RESET, COLOR_BOLD,
         phase_name, COLOR_RESET, status);
}

// Mostrar error
static void show_error(const char *message) {
  printf("\n  %s[✗]%s %sError:%s %s\n\n", COLOR_RED, COLOR_RESET, COLOR_BOLD,
         COLOR_RESET, message);
}

// Configurar timeout del socket
static int set_socket_timeout(int sockfd, int seconds) {
  struct timeval tv;
  tv.tv_sec = seconds;
  tv.tv_usec = 0;

  if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
    perror("setsockopt SO_RCVTIMEO");
    return -1;
  }
  return 0;
}

// Enviar PDU con reintentos
static int send_pdu_with_retry(int sockfd, struct sockaddr_in *server_addr,
                               uint8_t type, uint8_t seq_num,
                               const uint8_t *data, size_t data_len,
                               uint8_t expected_ack_seq, int show_progress) {
  uint8_t buffer[MAX_PDU_SIZE];
  uint8_t recv_buffer[MAX_PDU_SIZE];
  ssize_t pdu_size = 2 + (ssize_t)data_len;
  int retries = 0;

  buffer[0] = type;
  buffer[1] = seq_num;
  if (data_len > 0) {
    memcpy(buffer + 2, data, data_len);
  }

  while (retries < MAX_RETRIES) {
    if (retries > 0) {
      transfer_stats.retransmissions++;
    }

    ssize_t sent = sendto(sockfd, buffer, (size_t)pdu_size, 0,
                          (struct sockaddr *)server_addr, sizeof(*server_addr));

    if (sent < 0) {
      perror("sendto");
      return -1;
    }

    transfer_stats.packets_sent++;

    // Esperar ACK
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);

    ssize_t recv_len = recvfrom(sockfd, recv_buffer, MAX_PDU_SIZE, 0,
                                (struct sockaddr *)&from_addr, &from_len);

    if (recv_len < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        retries++;
        if (!show_progress) {
          printf("    %s⟳ Timeout, reintentando... (%d/%d)%s\n", COLOR_YELLOW,
                 retries, MAX_RETRIES, COLOR_RESET);
        }
        continue;
      }
      if (errno == EINTR) {
        continue;
      }
      perror("recvfrom");
      return -1;
    }

    if (!addr_equal(&from_addr, server_addr)) {
      continue;
    }

    if (recv_len < 2) {
      retries++;
      continue;
    }

    uint8_t recv_type = recv_buffer[0];
    uint8_t recv_seq = recv_buffer[1];

    if (recv_type != TYPE_ACK) {
      retries++;
      continue;
    }

    if (recv_seq != expected_ack_seq) {
      retries++;
      continue;
    }

    // ACK válido recibido
    if (recv_len > 2) {
      // Hay mensaje del servidor (probablemente error)
      if (type == TYPE_HELLO || type == TYPE_WRQ) {
        char error_msg[256];
        size_t msg_len = (size_t)(recv_len - 2);
        if (msg_len > 255)
          msg_len = 255;
        memcpy(error_msg, recv_buffer + 2, msg_len);
        error_msg[msg_len] = '\0';
        show_error(error_msg);
        return -1;
      }
    }

    return 0;
  }

  show_error("Máximo de reintentos alcanzado");
  return -1;
}

// Fase 1: Autenticación
static int phase_hello(int sockfd, struct sockaddr_in *server_addr,
                       const char *credentials) {
  show_phase_status("FASE 1: AUTENTICACIÓN", "Enviando credenciales...", 0);

  size_t cred_len = strlen(credentials);
  if (cred_len == 0 || cred_len > MAX_DATA_SIZE) {
    show_error("Credenciales inválidas");
    return -1;
  }

  if (send_pdu_with_retry(sockfd, server_addr, TYPE_HELLO, 0,
                          (const uint8_t *)credentials, cred_len, 0, 0) < 0) {
    return -1;
  }

  show_phase_status("FASE 1: AUTENTICACIÓN", "Completada", 1);
  return 0;
}

// Fase 2: Write Request
static int phase_wrq(int sockfd, struct sockaddr_in *server_addr,
                     const char *filename) {
  show_phase_status("FASE 2: WRITE REQUEST", "Solicitando permiso...", 0);

  size_t filename_len = strlen(filename);

  if (filename_len < 4 || filename_len > 10) {
    show_error("Filename debe tener entre 4 y 10 caracteres");
    return -1;
  }

  uint8_t buffer[12];
  strcpy((char *)buffer, filename);

  if (send_pdu_with_retry(sockfd, server_addr, TYPE_WRQ, 1, buffer,
                          filename_len + 1, 1, 0) < 0) {
    return -1;
  }

  show_phase_status("FASE 2: WRITE REQUEST", "Completada", 1);
  return 0;
}

// Fase 3: Transferencia de datos
static int phase_data_transfer(int sockfd, struct sockaddr_in *server_addr,
                               FILE *file) {
  show_phase_status("FASE 3: TRANSFERENCIA", "Enviando datos...", 0);
  printf("\n");

  uint8_t buffer[MAX_DATA_SIZE];
  uint8_t seq_num = 0;
  size_t total_sent = 0;
  uint8_t last_seq_sent = 0;
  int any_data_sent = 0;

  // Inicializar estadísticas
  gettimeofday(&transfer_stats.start_time, NULL);
  transfer_stats.bytes_sent = 0;

  while (1) {
    size_t bytes_read = fread(buffer, 1, MAX_DATA_SIZE, file);

    if (bytes_read == 0) {
      if (feof(file)) {
        if (!any_data_sent) {
          if (send_pdu_with_retry(sockfd, server_addr, TYPE_DATA, seq_num, NULL,
                                  0, seq_num, 1) < 0) {
            show_error("Error enviando DATA vacío");
            return -1;
          }
          last_seq_sent = seq_num;
          any_data_sent = 1;
          seq_num = 1 - seq_num;
        }
        break;
      }
      if (ferror(file)) {
        perror("fread");
        return -1;
      }
      continue;
    }

    // Mostrar progreso
    show_progress_bar("Progreso", total_sent, transfer_stats.total_bytes);

    if (send_pdu_with_retry(sockfd, server_addr, TYPE_DATA, seq_num, buffer,
                            bytes_read, seq_num, 1) < 0) {
      show_error("Error enviando datos");
      return -1;
    }

    total_sent += bytes_read;
    transfer_stats.bytes_sent = total_sent;
    last_seq_sent = seq_num;
    any_data_sent = 1;
    seq_num = 1 - seq_num;
  }

  // Mostrar 100%
  show_progress_bar("Progreso", transfer_stats.total_bytes,
                    transfer_stats.total_bytes);
  printf("\n\n");

  show_phase_status("FASE 3: TRANSFERENCIA", "Completada", 1);

  if (!any_data_sent) {
    return -1;
  }
  return last_seq_sent;
}

// Fase 4: Finalización
static int phase_finalize(int sockfd, struct sockaddr_in *server_addr,
                          const char *filename, uint8_t last_seq) {
  show_phase_status("FASE 4: FINALIZACIÓN", "Cerrando transferencia...", 0);

  uint8_t next_seq = (uint8_t)(1 - last_seq);
  size_t filename_len = strlen(filename);

  uint8_t buffer[12];
  strcpy((char *)buffer, filename);

  if (send_pdu_with_retry(sockfd, server_addr, TYPE_FIN, next_seq, buffer,
                          filename_len + 1, next_seq, 0) < 0) {
    return -1;
  }

  show_phase_status("FASE 4: FINALIZACIÓN", "Completada", 1);
  return 0;
}

// Mostrar resumen final
static void show_summary(void) {
  struct timeval now;
  gettimeofday(&now, NULL);
  double elapsed =
      (now.tv_sec - transfer_stats.start_time.tv_sec) +
      (now.tv_usec - transfer_stats.start_time.tv_usec) / 1000000.0;

  char bytes_str[32], speed_str[32];
  format_bytes(transfer_stats.bytes_sent, bytes_str, sizeof(bytes_str));
  format_speed(transfer_stats.bytes_sent, elapsed, speed_str,
               sizeof(speed_str));

  printf("\n");
  printf(
      "%s╔══════════════════════════════════════════════════════════════╗%s\n",
      COLOR_GREEN, COLOR_RESET);
  printf(
      "%s║%s             TRANSFERENCIA COMPLETADA CON ÉXITO            %s║%s\n",
      COLOR_GREEN, COLOR_BOLD, COLOR_GREEN, COLOR_RESET);
  printf(
      "%s╚══════════════════════════════════════════════════════════════╝%s\n",
      COLOR_GREEN, COLOR_RESET);
  printf("\n");
  printf("  %sEstadísticas:%s\n", COLOR_BOLD, COLOR_RESET);
  printf("    • Bytes enviados:     %s\n", bytes_str);
  printf("    • Tiempo transcurrido: %.2f segundos\n", elapsed);
  printf("    • Velocidad promedio:  %s\n", speed_str);
  printf("    • Paquetes enviados:   %d\n", transfer_stats.packets_sent);
  printf("    • Retransmisiones:     %d\n", transfer_stats.retransmissions);
  printf("\n");
}

int main(int argc, char *argv[]) {
  if (argc != 4) {
    fprintf(stderr, "Uso: %s <server_ip> <filename> <credentials>\n", argv[0]);
    fprintf(stderr, "Ejemplo: %s 192.168.1.100 test.txt mi_credencial\n",
            argv[0]);
    return 1;
  }

  const char *server_ip = argv[1];
  const char *filename = argv[2];
  const char *credentials = argv[3];

  // Abrir archivo
  FILE *file = fopen(filename, "rb");
  if (!file) {
    perror("fopen");
    return 1;
  }

  // Obtener tamaño del archivo
  transfer_stats.total_bytes = get_file_size(file);

  // Crear socket UDP
  int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0) {
    perror("socket");
    fclose(file);
    return 1;
  }

  // Configurar timeout
  if (set_socket_timeout(sockfd, TIMEOUT_SEC) < 0) {
    close(sockfd);
    fclose(file);
    return 1;
  }

  // Configurar dirección del servidor
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(SERVER_PORT);

  if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
    perror("inet_pton");
    close(sockfd);
    fclose(file);
    return 1;
  }

  // Mostrar header
  show_header(server_ip, filename);

  // Ejecutar protocolo
  int result = 0;

  // Fase 1: HELLO
  if (phase_hello(sockfd, &server_addr, credentials) < 0) {
    result = 1;
    goto cleanup;
  }

  // Fase 2: WRQ
  if (phase_wrq(sockfd, &server_addr, filename) < 0) {
    result = 1;
    goto cleanup;
  }

  // Fase 3: DATA
  int last_seq = phase_data_transfer(sockfd, &server_addr, file);
  if (last_seq < 0) {
    result = 1;
    goto cleanup;
  }

  // Fase 4: FIN
  if (phase_finalize(sockfd, &server_addr, filename, (uint8_t)last_seq) < 0) {
    result = 1;
    goto cleanup;
  }

  // Mostrar resumen
  show_summary();

cleanup:
  close(sockfd);
  fclose(file);
  return result;
}


