/*
 * Cliente UDP Stop & Wait File Transfer Protocol
 * Uso recomendado: compilar con el Makefile del proyecto.
 *    make client
 *    ./client <server_ip> <filename> <credencial>
 *
 * El filename se usa tanto como nombre local como remoto.
 */

#include <arpa/inet.h>
#include <asm-generic/errno-base.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "protocol.h"

long long current_time_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (ts.tv_sec * 1000LL) + (ts.tv_nsec / 1000000LL);
}

static int addr_equal(const struct sockaddr_in *a,
                      const struct sockaddr_in *b) {
  return a->sin_family == b->sin_family && a->sin_port == b->sin_port &&
         a->sin_addr.s_addr == b->sin_addr.s_addr;
}

static int send_pdu_with_retry(int sockfd, struct sockaddr_in *server_addr,
                               uint8_t type, uint8_t seq_num,
                               const uint8_t *data, size_t data_len,
                               uint8_t expected_ack_seq) {
  uint8_t buffer[MAX_PDU_SIZE];
  uint8_t recv_buffer[MAX_PDU_SIZE];
  ssize_t pdu_size = 2 + (ssize_t)data_len;
  int retries = 0;

  // Construir PDU
  buffer[0] = type;
  buffer[1] = seq_num;
  if (data_len > 0) {
    memcpy(buffer + 2, data, data_len);
  }

  while (retries < MAX_RETRIES) {

    // 1. ENVIAR PDU
    sendto(sockfd, buffer, pdu_size, 0, (struct sockaddr *)server_addr,
           sizeof(*server_addr));

    // 2. CALCULAR EL TIEMPO LÍMITE (DEADLINE)
    long long start_time = current_time_ms();
    long long deadline = start_time + TIMEOUT_MS;

    while (1) {
      long long now = current_time_ms();
      int time_left = (int)(deadline - now);

      // A. Verificamos si se acabó el tiempo REAL
      if (time_left <= 0) {
        printf("Timeout real alcanzado (retransmitiendo...)\n");
        break;
      }

      // B. Preparamos la estructura poll
      struct pollfd pfd;
      pfd.fd = sockfd;     // El socket que miramos
      pfd.events = POLLIN; // Nos interesa si hay datos para LEER

      // C. Esperamos solo el tiempo que nos queda (time_left)
      int rc = poll(&pfd, 1, time_left);

      if (rc < 0) {
        if (errno == EINTR)
          continue; // Nos interrumpieron, seguimos intentando
        perror("poll error");
        return -1;
      }

      if (rc == 0) {
        // Poll retornó 0, significa Timeout del poll.
        // El bucle volverá arriba, calculará time_left <= 0 y saldrá.
        continue;
      }

      // D. Si rc > 0, ¡HAY DATOS!
      if (pfd.revents & POLLIN) {

        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);

        // Como poll avisó, recvfrom es instantáneo
        ssize_t recv_len = recvfrom(sockfd, recv_buffer, MAX_PDU_SIZE, 0,
                                    (struct sockaddr *)&from_addr, &from_len);

        // --- VALIDACIONES (Stop & Wait estricto) ---

        // 1. Validar origen (Ignorar paquetes de intrusos)
        if (!addr_equal(&from_addr, server_addr)) {
          printf("Ignorando paquete de IP desconocida\n");
          continue;
        }

        // 2. Validar tamaño mínimo
        if (recv_len < 2)
          continue; // Muy corto, basura

        // 3. Validar ACK correcto
        if (recv_buffer[0] == TYPE_ACK && recv_buffer[1] == expected_ack_seq) {
          if (recv_len > 2) {
            // El servidor mandó ACK pero con payload -> Es un ERROR lógico
            // (ej. credenciales mal)
            printf("Error reportado por servidor: %.*s\n", (int)(recv_len - 2),
                   recv_buffer + 2);
            return -1; // Retornamos error para abortar
          }
          return 0; // Éxito limpio
        }

        // Si llegamos acá, es un ACK duplicado o incorrecto.
        // Lo ignoramos y el bucle sigue consumiendo el tiempo restante.
        printf("Ignorando ACK incorrecto (Seq recibida: %d)\n", recv_buffer[1]);
      }
    }

    // Si salimos del while(1) fue por timeout
    retries++;
  }

  printf("Máximo de reintentos alcanzado\n");
  return -1;
}

// Fase 1: Autenticación
static int phase_hello(int sockfd, struct sockaddr_in *server_addr,
                       const char *credentials) {
  printf("\n=== FASE 1: AUTENTICACIÓN ===\n");

  size_t cred_len = strlen(credentials);
  if (cred_len == 0 || cred_len > MAX_DATA_SIZE) {
    fprintf(stderr,
            "Credenciales inválidas: longitud debe ser entre 1 y %d bytes\n",
            MAX_DATA_SIZE);
    return -1;
  }
  if (send_pdu_with_retry(sockfd, server_addr, TYPE_HELLO, 0,
                          (const uint8_t *)credentials, cred_len, 0) < 0) {
    fprintf(stderr, "Error en fase de autenticación\n");
    return -1;
  }

  printf("Autenticación exitosa\n");
  return 0;
}

// Fase 2: Write Request
static int phase_wrq(int sockfd, struct sockaddr_in *server_addr,
                     const char *filename) {
  printf("\n=== FASE 2: WRITE REQUEST ===\n");

  size_t filename_len = strlen(filename);

  // Validar longitud del filename (4-10 caracteres)
  if (filename_len < 4 || filename_len > 10) {
    fprintf(stderr, "Filename debe tener entre 4 y 10 caracteres\n");
    return -1;
  }

  // Enviar filename con null terminator
  uint8_t buffer[12]; // 10 chars + null + margen
  strcpy((char *)buffer, filename);

  if (send_pdu_with_retry(sockfd, server_addr, TYPE_WRQ, 1, buffer,
                          filename_len + 1, 1) < 0) {
    fprintf(stderr, "Error en fase de Write Request\n");
    return -1;
  }

  printf("Write Request aceptado\n");
  return 0;
}

// Fase 3: Transferencia de datos
static int phase_data_transfer(int sockfd, struct sockaddr_in *server_addr,
                               FILE *file) {
  printf("\n=== FASE 3: TRANSFERENCIA DE DATOS ===\n");

  uint8_t buffer[MAX_DATA_SIZE];
  uint8_t seq_num = 0;
  size_t total_sent = 0;
  uint8_t last_seq_sent = 0;

  while (1) {
    size_t bytes_read = fread(buffer, 1, MAX_DATA_SIZE, file);

    if (bytes_read == 0) {
      if (feof(file)) {
        if (total_sent == 0) {
          // Archivo vacío: enviar un DATA vacío
          printf("Archivo vacío, enviando DATA vacío con Seq=%d\n", seq_num);
          if (send_pdu_with_retry(sockfd, server_addr, TYPE_DATA, seq_num, NULL,
                                  0, seq_num) < 0) {
            fprintf(stderr, "Error enviando DATA vacío\n");
            return -1;
          }
          last_seq_sent = seq_num;
        }
        printf("Archivo completamente leído\n");
        break;
      }
      if (ferror(file)) {
        perror("fread");
        return -1;
      }
      continue;
    }

    printf("Enviando DATA chunk: %zu bytes con Seq=%d\n", bytes_read, seq_num);

    if (send_pdu_with_retry(sockfd, server_addr, TYPE_DATA, seq_num, buffer,
                            bytes_read, seq_num) < 0) {
      fprintf(stderr, "Error enviando datos\n");
      return -1;
    }

    total_sent += bytes_read;
    last_seq_sent = seq_num;
    seq_num = 1 - seq_num; // Alternar 0 <-> 1
  }

  printf("Total enviado: %zu bytes\n", total_sent);
  return last_seq_sent;
}

// Fase 4: Finalización
static int phase_finalize(int sockfd, struct sockaddr_in *server_addr,
                          uint8_t last_seq) {
  printf("\n=== FASE 4: FINALIZACIÓN ===\n");

  uint8_t next_seq = 1 - last_seq;

  if (send_pdu_with_retry(sockfd, server_addr, TYPE_FIN, next_seq, NULL, 0,
                          next_seq) < 0) {
    fprintf(stderr, "Error en fase de finalización\n");
    return -1;
  }

  printf("Transferencia finalizada exitosamente\n");
  return 0;
}

int main(int argc, char *argv[]) {
  if (argc != 4) {
    fprintf(stderr, "Uso: %s <server_ip> <filename> <credencial>\n", argv[0]);
    fprintf(stderr, "Ejemplo: %s 127.0.0.1 test.bin test_credential\n",
            argv[0]);
    return 1;
  }

  const char *server_ip = argv[1];
  const char *filename_local = argv[2];
  const char *credentials = argv[3];
  const char *filename_remoto = filename_local;

  // Crear socket UDP
  int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0) {
    perror("socket");
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
    return 1;
  }

  printf("Conectando a %s:%d\n", server_ip, SERVER_PORT);

  // Ejecutar protocolo
  int result = 0;
  FILE *file = NULL;

  // Fase 1: HELLO
  if (phase_hello(sockfd, &server_addr, credentials) < 0) {
    result = 1;
    goto cleanup;
  }

  // Fase 2: WRQ
  if (phase_wrq(sockfd, &server_addr, filename_remoto) < 0) {
    result = 1;
    goto cleanup;
  }

  // Abrir archivo
  file = fopen(filename_local, "rb");
  if (!file) {
    perror("fopen");
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
  if (phase_finalize(sockfd, &server_addr, (uint8_t)last_seq) < 0) {
    result = 1;
    goto cleanup;
  }

  printf("\n✓ Transferencia completada exitosamente\n");

cleanup:
  close(sockfd);
  if (file)
    fclose(file);
  return result;
}
