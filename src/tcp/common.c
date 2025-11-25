#include "common.h"

#include <arpa/inet.h>
#include <bits/time.h>
#include <time.h>

uint64_t current_time_micros() {
  struct timespec ts;
  // IMPORTANTE: Debe ser REALTIME para comparar entre distintas máquinas
  clock_gettime(CLOCK_REALTIME, &ts);

  return (uint64_t)(ts.tv_sec) * 1000000 + (uint64_t)(ts.tv_nsec) / 1000;
}

// Conversión de host a network byte order para uint64_t
uint64_t hton64(uint64_t host) {
  // Comprobamos el endianness del sistema
  uint32_t test = 1;
  if (*(uint8_t *)&test == 1) {
    // Little endian: necesitamos invertir bytes
    return ((uint64_t)htonl((uint32_t)(host & 0xFFFFFFFF)) << 32) |
           htonl((uint32_t)(host >> 32));
  }
  // Big endian: no hay que cambiar nada
  return host;
}

// Conversión de network a host byte order para uint64_t
uint64_t ntoh64(uint64_t net) {
  return hton64(net); // Es simétrica
}
