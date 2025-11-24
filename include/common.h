#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stdio.h>
#include <sys/time.h>

/**
 * @brief Get current time in microseconds.
 * 
 * @return uint64_t Time in microseconds since epoch.
 */
uint64_t get_time_us(void);

// Macros for colored output (if not using ncurses)
#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_RESET   "\x1b[0m"

#define LOG_INFO(fmt, ...)  fprintf(stdout, ANSI_COLOR_CYAN "[INFO] " ANSI_COLOR_RESET fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  fprintf(stdout, ANSI_COLOR_YELLOW "[WARN] " ANSI_COLOR_RESET fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, ANSI_COLOR_RED "[ERROR] " ANSI_COLOR_RESET fmt "\n", ##__VA_ARGS__)
#define LOG_OK(fmt, ...)    fprintf(stdout, ANSI_COLOR_GREEN "[OK] " ANSI_COLOR_RESET fmt "\n", ##__VA_ARGS__)

#endif // COMMON_H
