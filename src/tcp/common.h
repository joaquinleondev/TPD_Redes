#ifndef TCP_COMMON_H
#define TCP_COMMON_H

#include <stdint.h>

// Retorna el tiempo actual en microsegundos (usando CLOCK_MONOTONIC)
uint64_t current_time_micros(void);

// Funciones de conversión de byte order para uint64_t (portabilidad)
uint64_t hton64(uint64_t host);
uint64_t ntoh64(uint64_t net);

#endif
