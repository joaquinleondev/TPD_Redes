#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

#define SERVER_PORT 20252

#define MIN_PAYLOAD_SIZE 500
#define MAX_PAYLOAD_SIZE 1000
#define DEFAULT_PAYLOAD_SIZE 800

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

// Enviar todos los bytes del buffer (maneja escrituras parciales)
static int send_all(int sockfd, const uint8_t *buf, size_t len) {
  size_t total_sent = 0;

  while (total_sent < len) {
    ssize_t n = send(sockfd, buf + total_sent, len - total_sent, 0);
    if (n < 0) {
      if (errno == EINTR) {
        if (!g_running) {
          return -1; // Interrupción por señal de terminación
        }
        continue; // Reintentar si fue otra interrupción
      }
      perror("send");
      return -1;
    }
    if (n == 0) {
      fprintf(stderr, "send devolvió 0, conexión cerrada inesperadamente\n");
      return -1;
    }
    total_sent += (size_t)n;
  }
  return 0;
}

// Dormir por un tiempo especificado en milisegundos, manejando interrupciones
static void sleep_ms(int ms) {
  struct timespec req, rem;
  req.tv_sec = ms / 1000;
  req.tv_nsec = (long)(ms % 1000) * 1000000L;

  while (nanosleep(&req, &rem) < 0) {
    if (errno == EINTR) {
      if (!g_running) {
        return; // Salir si recibimos señal de terminación
      }
      req = rem; // Continuar con el tiempo restante
    } else {
      perror("nanosleep");
      return;
    }
  }
}

static void print_usage(const char *progname) {
  fprintf(stderr,
          "Uso: %s <server_ip> -d <ms_entre_envios> -N <duracion_segundos>\n",
          progname);
  fprintf(stderr, "Ejemplo: %s 192.168.0.10 -d 50 -N 10\n", progname);
  fprintf(stderr, "\nOpciones:\n");
  fprintf(stderr, "  -d <ms>     Intervalo entre PDUs en milisegundos (>0)\n");
  fprintf(stderr, "  -N <seg>    Duración total del test en segundos (>0)\n");
}

// Parsear argumentos -d y -N
static int parse_args(int argc, char *argv[], const char **server_ip, int *d_ms,
                      int *N_seconds) {
  if (argc < 6) {
    return -1;
  }

  *server_ip = argv[1];
  *d_ms = -1;
  *N_seconds = -1;

  int i = 2;
  while (i < argc) {
    if (strcmp(argv[i], "-d") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "ERROR: -d requiere un valor\n");
        return -1;
      }
      char *endptr;
      long val = strtol(argv[i + 1], &endptr, 10);
      if (*endptr != '\0' || val <= 0 || val > 60000) {
        fprintf(stderr, "ERROR: -d debe ser un entero entre 1 y 60000\n");
        return -1;
      }
      *d_ms = (int)val;
      i += 2;
    } else if (strcmp(argv[i], "-N") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "ERROR: -N requiere un valor\n");
        return -1;
      }
      char *endptr;
      long val = strtol(argv[i + 1], &endptr, 10);
      if (*endptr != '\0' || val <= 0 || val > 86400) {
        fprintf(stderr, "ERROR: -N debe ser un entero entre 1 y 86400\n");
        return -1;
      }
      *N_seconds = (int)val;
      i += 2;
    } else {
      fprintf(stderr, "ERROR: Parámetro desconocido: %s\n", argv[i]);
      return -1;
    }
  }

  if (*d_ms <= 0) {
    fprintf(stderr, "ERROR: Falta el parámetro -d\n");
    return -1;
  }
  if (*N_seconds <= 0) {
    fprintf(stderr, "ERROR: Falta el parámetro -N\n");
    return -1;
  }

  return 0;
}

int main(int argc, char *argv[]) {
  const char *server_ip = NULL;
  int d_ms = 0;      // Milisegundos entre PDUs
  int N_seconds = 0; // Duración total en segundos

  if (parse_args(argc, argv, &server_ip, &d_ms, &N_seconds) < 0) {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  setup_signal_handlers();

  printf("=== Cliente TCP ===\n");
  printf("Servidor: %s:%d\n", server_ip, SERVER_PORT);
  printf("Intervalo entre envíos: %d ms\n", d_ms);
  printf("Duración total: %d s\n", N_seconds);
  printf("Tamaño de payload: %d bytes\n\n", DEFAULT_PAYLOAD_SIZE);

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
    fprintf(stderr, "ERROR: Dirección IP inválida: %s\n", server_ip);
    close(sockfd);
    return EXIT_FAILURE;
  }

  // Conectar al servidor
  printf("Conectando al servidor...\n");
  if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) <
      0) {
    perror("connect");
    close(sockfd);
    return EXIT_FAILURE;
  }

  printf("Conectado al servidor TCP.\n");

  // Preparar buffer de PDU (8 bytes timestamp + payload + 1 delimitador)
  const size_t payload_size = DEFAULT_PAYLOAD_SIZE;
  const size_t pdu_size = 8 + payload_size + 1;

  uint8_t *pdu = malloc(pdu_size);
  if (!pdu) {
    perror("malloc");
    close(sockfd);
    return EXIT_FAILURE;
  }

  // Rellenar el payload con datos (pattern fijo)
  memset(pdu + 8, 0x20, payload_size);
  pdu[8 + payload_size] = '|'; // Delimitador final

  // Usar CLOCK_REALTIME para medir duración (en microsegundos)
  uint64_t start_us = current_time_micros();
  uint64_t duration_us = (uint64_t)N_seconds * 1000000ULL;
  int pdus_sent = 0;

  printf("Comenzando a enviar PDUs...\n");
  printf("Presione Ctrl+C para terminar anticipadamente.\n\n");

  while (g_running) {
    // Verificar tiempo transcurrido
    uint64_t now_us = current_time_micros();
    uint64_t elapsed_us = now_us - start_us;

    if (elapsed_us >= duration_us) {
      printf("\nDuración total alcanzada (%.2f s), finalizando.\n",
             elapsed_us / 1000000.0);
      break;
    }

    // Origin Timestamp: se toma justo antes de enviar la PDU (en microsegundos)
    // Se envía en network byte order para portabilidad
    uint64_t origin_ts = (uint64_t)current_time_micros();
    uint64_t origin_ts_net = hton64(origin_ts);
    memcpy(pdu, &origin_ts_net, sizeof(origin_ts_net));

    // Enviar la PDU completa
    if (send_all(sockfd, pdu, pdu_size) < 0) {
      if (g_running) {
        fprintf(stderr, "Error enviando PDU, abortando.\n");
      }
      break;
    }

    pdus_sent++;

    // Mostrar progreso cada 100 PDUs o cada segundo
    if (pdus_sent % 100 == 0 || pdus_sent == 1) {
      printf("PDUs enviadas: %d (tiempo: %.1f s)\n", pdus_sent,
             elapsed_us / 1000000.0);
    }

    // Esperar antes de enviar la próxima PDU
    sleep_ms(d_ms);
  }

  // Estadísticas finales
  uint64_t end_us = current_time_micros();
  double total_time_s = (end_us - start_us) / 1000000.0;

  printf("\n=== Estadísticas del cliente ===\n");
  printf("PDUs enviadas: %d\n", pdus_sent);
  printf("Tiempo total: %.2f s\n", total_time_s);
  if (total_time_s > 0) {
    printf("Tasa promedio: %.2f PDUs/s\n", pdus_sent / total_time_s);
  }

  free(pdu);
  close(sockfd);
  printf("Cliente TCP finalizado.\n");
  return EXIT_SUCCESS;
}
