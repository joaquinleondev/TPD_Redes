/*
 * Cliente UDP Stop & Wait File Transfer Protocol
 * Uso recomendado: compilar con el Makefile del proyecto.
 *    make client
 *    ./client <server_ip> <filename> <credencial>
 *
 * El filename se usa tanto como nombre local como remoto.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "protocol.h"

static int addr_equal(const struct sockaddr_in *a,
                      const struct sockaddr_in *b) {
  return a->sin_family == b->sin_family && a->sin_port == b->sin_port &&
         a->sin_addr.s_addr == b->sin_addr.s_addr;
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
    // Enviar PDU
    printf("Enviando PDU: Type=%d, Seq=%d, Size=%zd (intento %d/%d)\n", type,
           seq_num, pdu_size, retries + 1, MAX_RETRIES);

    ssize_t sent = sendto(sockfd, buffer, (size_t)pdu_size, 0,
                          (struct sockaddr *)server_addr, sizeof(*server_addr));

    if (sent < 0) {
      perror("sendto");
      return -1;
    }

    // Esperar ACK
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);

    ssize_t recv_len = recvfrom(sockfd, recv_buffer, MAX_PDU_SIZE, 0,
                                (struct sockaddr *)&from_addr, &from_len);

    if (recv_len < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        printf("Timeout esperando ACK, reintentando...\n");
        retries++;
        continue;
      }
      if (errno == EINTR) {
        printf("recvfrom interrumpido por señal, reintentando...\n");
        continue;
      }
      perror("recvfrom");
      return -1;
    }

    if (!addr_equal(&from_addr, server_addr)) {
      printf("PDU recibida de origen desconocido, ignorando\n");
      continue;
    }

    // Validar ACK
    if (recv_len < 2) {
      printf("PDU inválida recibida (muy corta)\n");
      retries++;
      continue;
    }

    uint8_t recv_type = recv_buffer[0];
    uint8_t recv_seq = recv_buffer[1];

    if (recv_type != TYPE_ACK) {
      printf("Tipo de PDU inesperado: %d (esperaba ACK)\n", recv_type);
      retries++;
      continue;
    }

    if (recv_seq != expected_ack_seq) {
      printf("Número de secuencia incorrecto: %d (esperaba %d), ignorando\n",
             recv_seq, expected_ack_seq);
      continue;
    }

    // ACK válido recibido
    printf("ACK recibido correctamente: Seq=%d\n", recv_seq);

    // Verificar si hay mensaje de error
    if (recv_len > 2) {
      printf("Mensaje del servidor: %.*s\n", (int)(recv_len - 2),
             recv_buffer + 2);
      // Si hay payload, podría ser un error
      return -1; // Error en autenticación o WRQ
    }

    return 0; // Éxito
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

  // Validar longitud del filename (4-100 caracteres)
  if (filename_len < 4 || filename_len > 100) {
    fprintf(stderr, "Filename debe tener entre 4 y 100 caracteres\n");
    return -1;
  }

  // Enviar filename con null terminator
  uint8_t buffer[102]; // 100 chars + null + margen
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
  int any_data_sent = 0;

  while (1) {
    size_t bytes_read = fread(buffer, 1, MAX_DATA_SIZE, file);

    if (bytes_read == 0) {
      if (feof(file)) {
        if (!any_data_sent) {
          printf("Archivo vacío, enviando DATA vacío con Seq=%d\n", seq_num);
          if (send_pdu_with_retry(sockfd, server_addr, TYPE_DATA, seq_num, NULL,
                                  0, seq_num) < 0) {
            fprintf(stderr, "Error enviando DATA vacío\n");
            return -1;
          }
          last_seq_sent = seq_num;
          any_data_sent = 1;
          seq_num = 1 - seq_num;
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
    any_data_sent = 1;
    seq_num = 1 - seq_num; // Alternar 0 <-> 1
  }

  printf("Total enviado: %zu bytes\n", total_sent);
  if (!any_data_sent) {
    return -1;
  }
  return last_seq_sent; // Retornar último seq_num usado
}

// Fase 4: Finalización
static int phase_finalize(int sockfd, struct sockaddr_in *server_addr,
                          const char *filename, uint8_t last_seq) {
  printf("\n=== FASE 4: FINALIZACIÓN ===\n");

  uint8_t next_seq = 1 - last_seq; // Incrementar seq_num
  size_t filename_len = strlen(filename);

  uint8_t buffer[12];
  if (filename_len >= sizeof(buffer)) {
    fprintf(stderr, "Filename too long for finalize buffer\n");
    return -1;
  }
  strcpy((char *)buffer, filename);

  if (send_pdu_with_retry(sockfd, server_addr, TYPE_FIN, next_seq, buffer,
                          filename_len + 1, next_seq) < 0) {
    fprintf(stderr, "Error en fase de finalización\n");
    return -1;
  }

  printf("Transferencia finalizada exitosamente\n");
  return 0;
}

int main(int argc, char *argv[]) {
  if (argc != 4) {
    fprintf(stderr, "Uso: %s <server_ip> <filename> <credencial>\n", argv[0]);
    fprintf(stderr,
            "Ejemplo: %s 127.0.0.1 test.bin test_credential\n",
            argv[0]);
    return 1;
  }

  const char *server_ip = argv[1];
  const char *filename_local = argv[2];
  const char *credentials = argv[3];
  const char *filename_remoto = filename_local;

  // Abrir archivo
  FILE *file = fopen(filename_local, "rb");
  if (!file) {
    perror("fopen");
    return 1;
  }

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

  printf("Conectando a %s:%d\n", server_ip, SERVER_PORT);

  // Ejecutar protocolo
  int result = 0;

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

  // Fase 3: DATA
  int last_seq = phase_data_transfer(sockfd, &server_addr, file);
  if (last_seq < 0) {
    result = 1;
    goto cleanup;
  }

  // Fase 4: FIN
  if (phase_finalize(sockfd, &server_addr, filename_remoto, (uint8_t)last_seq) <
      0) {
    result = 1;
    goto cleanup;
  }

  printf("\n✓ Transferencia completada exitosamente\n");

cleanup:
  close(sockfd);
  fclose(file);
  return result;
}


