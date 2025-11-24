#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "protocol.h"

// Estructura para mantener estado de cada cliente
typedef struct {
  struct sockaddr_in addr;
  ClientState state;
  uint8_t expected_seq;
  char filename[256];
  FILE *file;
  time_t last_activity;
  int active;
  size_t bytes_received;
  uint8_t last_ack_seq;
  int has_last_ack;
} ClientSession;

// Lista de credenciales válidas
static char valid_credentials[MAX_CREDENTIALS][256];
static int num_credentials = 0;

// Pool de sesiones de clientes
static ClientSession clients[MAX_CLIENTS];

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
    // Eliminar newline
    size_t len = strlen(valid_credentials[num_credentials]);
    if (len > 0 && valid_credentials[num_credentials][len - 1] == '\n') {
      valid_credentials[num_credentials][len - 1] = '\0';
    }
    num_credentials++;
  }

  fclose(f);
  printf("Cargadas %d credenciales\n", num_credentials);
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

  // Buscar sesión existente
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clients[i].active && addr_equal(&clients[i].addr, addr)) {
      clients[i].last_activity = now;
      return &clients[i];
    }
    if (!clients[i].active && !free_slot) {
      free_slot = &clients[i];
    }
  }

  // Crear nueva sesión si hay espacio
  if (free_slot) {
    memset(free_slot, 0, sizeof(ClientSession));
    free_slot->addr = *addr;
    free_slot->state = STATE_IDLE;
    free_slot->expected_seq = 0;
    free_slot->last_activity = now;
    free_slot->active = 1;
    free_slot->file = NULL;

    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
    printf("Nueva sesión para %s:%d\n", ip, ntohs(addr->sin_port));

    return free_slot;
  }

  return NULL; // No hay espacio
}

// Liberar recursos de una sesión
static void cleanup_session(ClientSession *session) {
  if (session->file) {
    fclose(session->file);
    session->file = NULL;
  }

  char ip[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &session->addr.sin_addr, ip, sizeof(ip));
  printf("Sesión liberada para %s:%d (bytes recibidos: %zu)\n", ip,
         ntohs(session->addr.sin_port), session->bytes_received);

  session->active = 0;
}

// Limpiar sesiones inactivas
static void cleanup_inactive_sessions(void) {
  time_t now = time(NULL);

  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clients[i].active &&
        (now - clients[i].last_activity) > CLIENT_TIMEOUT) {
      printf("Timeout de sesión\n");
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

  ssize_t sent = sendto(sockfd, buffer, pdu_size, 0, (struct sockaddr *)addr,
                        sizeof(*addr));
  if (sent < 0) {
    perror("sendto ACK");
  }

  char ip[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
  printf("ACK enviado a %s:%d - Seq=%d%s%s, DataLen=%zu\n", ip,
         ntohs(addr->sin_port), seq_num, error_msg ? " Error: " : "",
         error_msg ? error_msg : "", pdu_size - 2);
}

// Manejar PDU HELLO
static void handle_hello(int sockfd, struct sockaddr_in *addr, uint8_t *data,
                         size_t data_len, uint8_t seq_num) {
  ClientSession *session = find_or_create_session(addr);
  if (!session) {
    printf("Sin espacio para nuevos clientes\n");
    return;
  }

  // Validar sequence number
  if (seq_num != 0) {
    printf("HELLO con Seq != 0, descartando\n");
    return;
  }

  // Si la sesión no está en IDLE, tratamos como posible retransmisión
  if (session->state != STATE_IDLE) {
    if (session->has_last_ack && session->last_ack_seq == 0) {
      printf("HELLO duplicado, reenviando ACK\n");
      send_ack(sockfd, addr, 0, NULL);
    } else {
      printf("HELLO recibido en estado incorrecto, descartando\n");
    }
    return;
  }

  // Solo aquí extraemos / mostramos credenciales porque el estado es válido
  char credentials[256];
  size_t cred_len = (data_len < 255) ? data_len : 255;
  memcpy(credentials, data, cred_len);
  credentials[cred_len] = '\0';

  printf("Autenticación recibida: '%s'\n", credentials);

  // Validar credenciales
  if (!is_valid_credential(credentials)) {
    send_ack(sockfd, addr, 0, "Invalid credentials");
    cleanup_session(session);
    return;
  }

  // Autenticación exitosa
  session->state = STATE_AUTHENTICATED;
  session->expected_seq = 1; // Siguiente debe ser WRQ con seq=1
  session->last_ack_seq = 0;
  session->has_last_ack = 1;
  send_ack(sockfd, addr, 0, NULL);
}

// Manejar PDU WRQ
static void handle_wrq(int sockfd, struct sockaddr_in *addr, uint8_t *data,
                       size_t data_len, uint8_t seq_num) {
  ClientSession *session = find_or_create_session(addr);
  if (!session) {
    return;
  }

  // Validar sequence number
  if (seq_num != 1) {
    printf("WRQ con Seq != 1, descartando\n");
    return;
  }

  // Extraer filename (máx 10 caracteres + null terminator)
  char filename[12]; // 10 chars + null + margen
  size_t fn_len = 0;

  // Buscar el null terminator y copiar
  for (size_t i = 0; i < data_len && i < sizeof(filename) - 1; i++) {
    if (data[i] == '\0') {
      break;
    }
    filename[i] = (char)data[i];
    fn_len = i + 1;
  }
  filename[fn_len] = '\0';

  printf("Solicitud de escritura: '%s'\n", filename);

  if (session->state == STATE_AUTHENTICATED) {
    // Validar longitud (4-10 caracteres)
    if (fn_len < 4 || fn_len > 10) {
      send_ack(sockfd, addr, 1, "Filename length must be 4-10 chars");
      return;
    }

    // Validar caracteres ASCII permitidos
    for (size_t j = 0; j < fn_len; j++) {
      unsigned char c = (unsigned char)filename[j];
      if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') || c == '_' || c == '-' || c == '.')) {
        send_ack(sockfd, addr, 1, "Invalid filename characters");
        return;
      }
    }

    // Abrir archivo dentro de uploads/ para mantener todo ordenado
    if (mkdir("uploads", 0755) < 0 && errno != EEXIST) {
      perror("mkdir uploads");
      send_ack(sockfd, addr, 1, "Server error");
      return;
    }

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "uploads/%s", filename);
    session->file = fopen(filepath, "wb");

    if (!session->file) {
      send_ack(sockfd, addr, 1, "Cannot create file");
      return;
    }

    strcpy(session->filename, filename);
    session->state = STATE_READY_TO_TRANSFER;
    session->expected_seq = 0; // Primer DATA debe tener seq=0
    session->has_last_ack = 1;
    session->last_ack_seq = 1;
    send_ack(sockfd, addr, 1, NULL);

  } else if (session->state == STATE_READY_TO_TRANSFER ||
             session->state == STATE_TRANSFERRING) {
    // Posible WRQ duplicado: comprobar que el filename coincide
    if (strcmp(session->filename, filename) == 0) {
      printf("WRQ duplicado para '%s', reenviando ACK\n", filename);
      send_ack(sockfd, addr, 1, NULL);
    } else {
      send_ack(sockfd, addr, 1, "Filename mismatch");
    }
  } else {
    printf("WRQ en estado incorrecto, descartando\n");
  }
}

// Manejar PDU DATA
static void handle_data(int sockfd, struct sockaddr_in *addr, uint8_t *data,
                        size_t data_len, uint8_t seq_num) {
  ClientSession *session = find_or_create_session(addr);
  if (!session) {
    return;
  }

  // Validar estado
  if (session->state != STATE_READY_TO_TRANSFER &&
      session->state != STATE_TRANSFERRING) {
    printf("DATA sin WRQ previo, descartando\n");
    return;
  }

  // Validar sequence number
  if (seq_num == session->expected_seq) {
    // Escribir datos nuevos
    if (data_len > 0) {
      size_t written = fwrite(data, 1, data_len, session->file);
      if (written != data_len) {
        printf("Error escribiendo archivo\n");
        cleanup_session(session);
        return;
      }
      session->bytes_received += written;
    }

    // Enviar ACK para nuevo DATA
    send_ack(sockfd, addr, seq_num, NULL);

    // Actualizar estado y último ACK
    session->state = STATE_TRANSFERRING;
    session->expected_seq = 1 - seq_num; // Alternar 0 <-> 1
    session->last_ack_seq = seq_num;
    session->has_last_ack = 1;
  } else {
    // Seq incorrecto: puede ser duplicado o error
    printf("Seq incorrecto: recibido=%d, esperado=%d\n", seq_num,
           session->expected_seq);

    // Si coincide con el último ACK, es un duplicado (retransmisión del
    // cliente)
    if (session->has_last_ack && seq_num == session->last_ack_seq) {
      printf("DATA duplicado (Seq=%d), reenviando ACK\n", seq_num);
      send_ack(sockfd, addr, session->last_ack_seq, NULL);
    }
  }
}

// Manejar PDU FIN (sin payload, solo type + seq_num)
static void handle_fin(int sockfd, struct sockaddr_in *addr, uint8_t seq_num) {
  ClientSession *session = find_or_create_session(addr);
  if (!session) {
    return;
  }

  if (session->state == STATE_TRANSFERRING) {
    // Validar sequence number (debe ser el siguiente esperado)
    if (seq_num != session->expected_seq) {
      printf("FIN con Seq incorrecto: recibido=%d, esperado=%d\n", seq_num,
             session->expected_seq);
      return;
    }

    printf("Finalización recibida: '%s', total: %zu bytes\n", session->filename,
           session->bytes_received);

    // Cerrar archivo y enviar ACK final
    if (session->file) {
      fclose(session->file);
      session->file = NULL;
    }
    send_ack(sockfd, addr, seq_num, NULL);
    session->state = STATE_COMPLETED;
    session->last_ack_seq = seq_num;
    session->has_last_ack = 1;

  } else if (session->state == STATE_COMPLETED) {
    // FIN duplicado: reenviar ACK si el seq coincide
    if (session->has_last_ack && seq_num == session->last_ack_seq) {
      printf("FIN duplicado para '%s', reenviando ACK\n", session->filename);
      send_ack(sockfd, addr, seq_num, NULL);
    }
  } else {
    printf("FIN en estado incorrecto (%d), descartando\n", session->state);
  }
}

int main(int argc, char *argv[]) {
  if (argc != 2) {
    fprintf(stderr, "Uso: %s <credentials_file>\n", argv[0]);
    return 1;
  }

  // Cargar credenciales
  if (load_credentials(argv[1]) < 0) {
    return 1;
  }
  // Inicializar pool de clientes
  memset(clients, 0, sizeof(clients));

  // Crear socket UDP
  int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0) {
    perror("socket");
    return 1;
  }

  // Permitir reutilización de dirección
  int reuse = 1;
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
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
    perror("bind");
    close(sockfd);
    return 1;
  }

  printf("Servidor escuchando en puerto %d\n", SERVER_PORT);
  printf("Máximo de clientes concurrentes: %d\n", MAX_CLIENTS);

  // Loop principal
  uint8_t buffer[MAX_PDU_SIZE];

  while (1) {
    cleanup_inactive_sessions();

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    struct pollfd pfd;
    pfd.fd = sockfd;
    pfd.events = POLLIN;

    // Esperar hasta 1000ms (1 segundo)
    int ret = poll(&pfd, 1, 1000);

    if (ret < 0) {
      if (errno == EINTR)
        continue;
      perror("poll");
      break;
    }

    if (ret == 0) {
      // Timeout del poll: No llegó nada, volvemos arriba a limpiar sesiones
      continue;
    }

    if (pfd.revents & POLLIN) {
      // Ahora sí, recvfrom es seguro y no bloqueará
      ssize_t recv_len = recvfrom(sockfd, buffer, MAX_PDU_SIZE, 0,
                                  (struct sockaddr *)&client_addr, &client_len);

      if (recv_len < 0) {
        if (errno == EINTR)
          continue;
        perror("recvfrom");
        break;
      }

      if (recv_len < 2) {
        printf("PDU demasiado corta, descartando\n");
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
        handle_fin(sockfd, &client_addr, seq_num);
        break;
      default:
        printf("Tipo de PDU desconocido: %d\n", type);
        break;
      }
    }
  }

  // Cleanup (solo se ejecuta si salimos del loop por error)
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clients[i].active) {
      cleanup_session(&clients[i]);
    }
  }

  close(sockfd);
  return 0;
}