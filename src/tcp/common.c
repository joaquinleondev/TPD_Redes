#include <stdint.h>
#include <sys/time.h>
#include <time.h>

long long current_time_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (ts.tv_sec * 1000LL) + (ts.tv_nsec / 1000000LL);
}