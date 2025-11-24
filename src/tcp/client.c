/*
 * tcp_client.c
 *
 * Cliente TCP para medir one-way delay.
 * Uso recomendado: compilar con el Makefile del proyecto:
 *    make tcp_client
 *    ./tcp_client <server_ip> -d <ms_entre_envios> -N <duracion_segundos>
 *
 * Ejemplo: ./tcp_client 192.168.0.10 -d 50 -N 10
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
#include <time.h>
#include <unistd.h>

#define SERVER_PORT 20252

#define MIN_PAYLOAD_SIZE 500
#define MAX_PAYLOAD_SIZE 1000
#define DEFAULT_PAYLOAD_SIZE 800 // cualquier valor entre 500 y 1000 sirve

#include "common.h"

// Enviar todos los bytes del buffer (maneja escrituras parciales)
static int send_all(int sockfd, const uint8_t *buf, size_t len) {
  size_t total_sent = 0;

  while (total_sent < len) {
    ssize_t n = send(sockfd, buf + total_sent, len - total_sent, 0);
    if (n < 0) {
      if (errno == EINTR) {
        continue; // reintentar si fue interrumpido
      }
      perror("send");
      return -1;
    }
    if (n == 0) {
      // No debería pasar en TCP salvo cierre raro
      fprintf(stderr, "send devolvió 0, conexión cerrada inesperadamente\n");
      return -1;
    }
    total_sent += (size_t)n;
  }
  return 0;
}

static void print_usage(const char *progname) {
  fprintf(stderr,
          "Uso: %s <server_ip> -d <ms_entre_envios> -N <duracion_segundos>\n",
          progname);
  fprintf(stderr, "Ejemplo: %s 192.168.0.10 -d 50 -N 10\n", progname);
}

// Parsear argumentos -d y -N
static int parse_args(int argc, char *argv[], const char **server_ip, int *d_ms,
                      int *N_seconds) {
  if (argc < 5) {
    return -1;
  }

  *server_ip = argv[1];
  *d_ms = -1;
  *N_seconds = -1;

  int i = 2;
  while (i < argc) {
    if (strcmp(argv[i], "-d") == 0) {
      if (i + 1 >= argc)
        return -1;
      *d_ms = atoi(argv[i + 1]);
      i += 2;
    } else if (strcmp(argv[i], "-N") == 0) {
      if (i + 1 >= argc)
        return -1;
      *N_seconds = atoi(argv[i + 1]);
      i += 2;
    } else {
      // Parámetro desconocido
      return -1;
    }
  }

  if (*d_ms <= 0 || *N_seconds <= 0) {
    return -1;
  }

  return 0;
}

int main(int argc, char *argv[]) {
  const char *server_ip = NULL;
  int d_ms = 0;      // milisegundos entre PDUs
  int N_seconds = 0; // duración total en segundos

  if (parse_args(argc, argv, &server_ip, &d_ms, &N_seconds) < 0) {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  printf("Configuración cliente TCP:\n");
  printf("  Servidor: %s:%d\n", server_ip, SERVER_PORT);
  printf("  Intervalo entre envíos: %d ms\n", d_ms);
  printf("  Duración total: %d s\n", N_seconds);

  // Crear socket TCP
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    perror("socket");
    return EXIT_FAILURE;
  }

  // Configurar dirección del servidor
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(SERVER_PORT);

  if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
    perror("inet_pton");
    close(sockfd);
    return EXIT_FAILURE;
  }

  // Conectar al servidor
  if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) <
      0) {
    perror("connect");
    close(sockfd);
    return EXIT_FAILURE;
  }

  printf("Conectado al servidor TCP.\n");

  // Armar buffer de PDU (8 bytes timestamp + payload + 1 delimitador)
  const size_t payload_size = DEFAULT_PAYLOAD_SIZE;
  const size_t pdu_size = 8 + payload_size + 1;

  if (payload_size < MIN_PAYLOAD_SIZE || payload_size > MAX_PAYLOAD_SIZE) {
    fprintf(stderr, "Payload_size inválido en el código (%zu)\n", payload_size);
    close(sockfd);
    return EXIT_FAILURE;
  }

  uint8_t *pdu = malloc(pdu_size);
  if (!pdu) {
    perror("malloc");
    close(sockfd);
    return EXIT_FAILURE;
  }

  // Rellenar el payload una sola vez con 0x20 (espacios)
  memset(pdu + 8, 0x20, payload_size);
  pdu[8 + payload_size] = '|'; // delimitador final (ASCII 124)

  struct timeval start_tv, now_tv;
  if (gettimeofday(&start_tv, NULL) < 0) {
    perror("gettimeofday");
    free(pdu);
    close(sockfd);
    return EXIT_FAILURE;
  }

  printf("Comenzando a enviar PDUs...\n");

  while (1) {
    // Verificar tiempo transcurrido
    if (gettimeofday(&now_tv, NULL) < 0) {
      perror("gettimeofday");
      break;
    }

    double elapsed = (now_tv.tv_sec - start_tv.tv_sec) +
                     (now_tv.tv_usec - start_tv.tv_usec) / 1000000.0;

    if (elapsed >= (double)N_seconds) {
      printf("Duración total alcanzada (%.2f s), finalizando.\n", elapsed);
      break;
    }

    // Origin Timestamp: se toma justo antes de enviar la PDU
    uint64_t origin_ts = current_time_ms();
    memcpy(pdu, &origin_ts, sizeof(origin_ts));

    // Enviar la PDU completa (manejo de escrituras parciales)
    if (send_all(sockfd, pdu, pdu_size) < 0) {
      fprintf(stderr, "Error enviando PDU, abortando.\n");
      break;
    }

    // Esperar d_ms milisegundos antes de mandar la próxima (nanosleep)
    struct timespec ts;
    ts.tv_sec = d_ms / 1000;
    ts.tv_nsec = (long)(d_ms % 1000) * 1000000L; // ms -> ns
    nanosleep(&ts, NULL);
  }

  free(pdu);
  close(sockfd);
  printf("Cliente TCP finalizado.\n");
  return EXIT_SUCCESS;
}
