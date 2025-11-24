#ifndef UDP_PROTOCOL_H
#define UDP_PROTOCOL_H

#include <stdint.h>

// Puerto común del servidor UDP
#define SERVER_PORT 20252

// Tamaños de datos
// 1500 MTU - IP header (20) - UDP header (8) - PDU header (2) = 1470
#define MAX_DATA_SIZE 1024
#define MAX_PDU_SIZE (2 + MAX_DATA_SIZE)

// Tiempo de espera y reintentos (lado cliente)
#define TIMEOUT_SEC 3
#define MAX_RETRIES 15

// PDU Types
#define TYPE_HELLO 1
#define TYPE_WRQ 2
#define TYPE_DATA 3
#define TYPE_ACK 4
#define TYPE_FIN 5

// Estados del cliente/servidor (compartidos conceptualmente)
typedef enum {
  STATE_IDLE = 0,
  STATE_AUTHENTICATED,
  STATE_READY_TO_TRANSFER,
  STATE_TRANSFERRING,
  STATE_COMPLETED,
} ClientState;

// PDU genérica
typedef struct {
  uint8_t type;
  uint8_t seq_num;
  uint8_t data[MAX_DATA_SIZE];
} PDU;

#define TIMEOUT_MS (TIMEOUT_SEC * 1000L)
#define MAX_CLIENTS 10    // Definir según necesidad
#define CLIENT_TIMEOUT 60 // Timeout de inactividad en segundos
#define MAX_CREDENTIALS 100

#endif // UDP_PROTOCOL_H