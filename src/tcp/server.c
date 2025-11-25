#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common.h"

#define SERVER_PORT 20252

#define MIN_PAYLOAD_SIZE 500
#define MAX_PAYLOAD_SIZE 1000
// Tamaño mínimo de PDU: 8 bytes timestamp + 500 payload + 1 delimitador
#define MIN_PDU_SIZE (8 + MIN_PAYLOAD_SIZE + 1)
#define MAX_PDU_SIZE (8 + MAX_PAYLOAD_SIZE + 1)

// Buffer para lecturas parciales de TCP (debe poder contener varias PDUs)
#define RECV_BUF_SIZE 16384

// Flag para shutdown graceful
static volatile sig_atomic_t g_running = 1;

static void signal_handler(int sig) {
  (void)sig;
  g_running = 0;
}

static void setup_signal_handlers(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;

  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
}

int main(int argc, char *argv[]) {
  const char *csv_filename = "one_way_delay.csv";
  if (argc >= 2) {
    csv_filename = argv[1];
  }

  setup_signal_handlers();

  FILE *csv = fopen(csv_filename, "w");
  if (!csv) {
    perror("fopen CSV");
    return EXIT_FAILURE;
  }

  // Header CSV para análisis posterior (mediciones en microsegundos)
  fprintf(csv, "measurement,one_way_delay_us\n");
  fflush(csv);

  // Crear socket TCP
  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    perror("socket");
    fclose(csv);
    return EXIT_FAILURE;
  }

  // Permitir reutilizar el puerto inmediatamente
  int optval = 1;
  if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) <
      0) {
    perror("setsockopt SO_REUSEADDR");
    close(listen_fd);
    fclose(csv);
    return EXIT_FAILURE;
  }

  // Bind al puerto en todas las interfaces
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(SERVER_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind");
    close(listen_fd);
    fclose(csv);
    return EXIT_FAILURE;
  }

  if (listen(listen_fd, 1) < 0) {
    perror("listen");
    close(listen_fd);
    fclose(csv);
    return EXIT_FAILURE;
  }

  printf("Servidor TCP escuchando en puerto %d\n", SERVER_PORT);
  printf("Logueando one-way delay en: %s\n", csv_filename);
  printf("Presione Ctrl+C para terminar.\n\n");

  // Aceptar un único cliente
  struct sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);
  int conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
  if (conn_fd < 0) {
    if (errno == EINTR && !g_running) {
      printf("\nServidor interrumpido antes de recibir conexión.\n");
    } else {
      perror("accept");
    }
    close(listen_fd);
    fclose(csv);
    return (errno == EINTR) ? EXIT_SUCCESS : EXIT_FAILURE;
  }

  char client_ip[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
  printf("Cliente conectado desde %s:%d\n", client_ip,
         ntohs(client_addr.sin_port));

  uint8_t recv_buf[RECV_BUF_SIZE];
  size_t recv_len = 0;
  int measurement_idx = 0;
  int invalid_pdus = 0;

  while (g_running) {
    // Verificar si hay espacio en el buffer
    if (recv_len >= sizeof(recv_buf)) {
      // Buffer lleno sin delimitador encontrado: error de protocolo
      fprintf(stderr, "ERROR: Buffer lleno sin encontrar delimitador. "
                      "Posible corrupción de protocolo. Limpiando buffer.\n");
      recv_len = 0;
      invalid_pdus++;
      continue;
    }

    // Leer del socket TCP (pueden llegar PDUs parciales o múltiples juntas)
    ssize_t n =
        recv(conn_fd, recv_buf + recv_len, sizeof(recv_buf) - recv_len, 0);
    if (n < 0) {
      if (errno == EINTR) {
        continue; // Reintentar si fue interrumpido por señal
      }
      perror("recv");
      break;
    }
    if (n == 0) {
      printf("Cliente cerró la conexión.\n");
      break;
    }

    recv_len += (size_t)n;

    // Procesar todas las PDUs completas que haya en el buffer
    while (recv_len > 0) {
      // Buscar el delimitador '|' dentro del buffer, pero ignorar cualquier '|'
      // que aparezca antes de MIN_PDU_SIZE (sería un byte '|' dentro del
      // timestamp)
      size_t delim_pos;
      int found = 0;
      // Empezar la búsqueda desde MIN_PDU_SIZE - 1 (posición mínima válida para
      // '|')
      size_t search_start = (MIN_PDU_SIZE > 0) ? (MIN_PDU_SIZE - 1) : 0;
      for (delim_pos = search_start; delim_pos < recv_len; delim_pos++) {
        if (recv_buf[delim_pos] == '|') {
          found = 1;
          break;
        }
      }

      if (!found) {
        // No hay PDU completa aún, esperar más datos
        break;
      }

      size_t pdu_len = delim_pos + 1; // Incluye el delimitador

      // Validar tamaño máximo
      if (pdu_len > MAX_PDU_SIZE) {
        fprintf(stderr,
                "WARN: PDU demasiado larga (%zu bytes, máximo %d). "
                "Descartando.\n",
                pdu_len, MAX_PDU_SIZE);
        invalid_pdus++;
        size_t remaining = recv_len - pdu_len;
        if (remaining > 0) {
          memmove(recv_buf, recv_buf + pdu_len, remaining);
        }
        recv_len = remaining;
        continue;
      }

      // PDU válida: tomar Destination Timestamp AHORA (en microsegundos)
      uint64_t dest_ts = current_time_micros();

      // Extraer Origin Timestamp de los primeros 8 bytes (network byte order)
      uint64_t origin_ts_net;
      memcpy(&origin_ts_net, recv_buf, sizeof(origin_ts_net));
      uint64_t origin_ts = ntoh64(origin_ts_net);

      // Calcular one-way delay en microsegundos
      uint64_t delay_us = 0;
      if ((uint64_t)dest_ts >= origin_ts) {
        delay_us = (uint64_t)dest_ts - origin_ts;
      } else {
        // Timestamp negativo (relojes desincronizados o wrap-around)
        delay_us = (uint64_t)origin_ts - (uint64_t)dest_ts;
      }

      measurement_idx++;

      // Loguear en CSV (segundos)
      fprintf(csv, "%d,%.6f\n", measurement_idx, delay_us / 1000000.0);
      fflush(csv);

      printf("Medición %d: delay = %" PRIu64 " us (%.3f ms)\n", measurement_idx,
             delay_us, delay_us / 1000.0);

      // Mover el resto de bytes para la próxima PDU
      size_t remaining = recv_len - pdu_len;
      if (remaining > 0) {
        memmove(recv_buf, recv_buf + pdu_len, remaining);
      }
      recv_len = remaining;
    }
  }

  // Estadísticas finales
  printf("\n=== Estadísticas del servidor ===\n");
  printf("PDUs válidas recibidas: %d\n", measurement_idx);
  printf("PDUs inválidas/descartadas: %d\n", invalid_pdus);

  close(conn_fd);
  close(listen_fd);
  fclose(csv);

  printf("Servidor TCP finalizado.\n");
  return EXIT_SUCCESS;
}
