/*
 * tcp_server.c
 *
 * Servidor TCP para medir one-way delay.
 * Escucha en el puerto 20252, acepta un cliente y loguea los delays en un CSV.
 *
 * Uso: ./tcp_server [output.csv]
 *   Si no se especifica archivo, usa "one_way_delay.csv".
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

#define SERVER_PORT 20252

#define MIN_PAYLOAD_SIZE 500
#define MAX_PAYLOAD_SIZE 1000
#define MAX_PDU_SIZE (8 + MAX_PAYLOAD_SIZE + 1)

// Buffer para lecturas parciales de TCP
#define RECV_BUF_SIZE 8192

// Obtener tiempo actual en microsegundos desde epoch
static uint64_t get_time_us(void) {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) < 0) {
        perror("gettimeofday");
        return 0;
    }
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

int main(int argc, char *argv[]) {
    const char *csv_filename = "one_way_delay.csv";
    if (argc >= 2) {
        csv_filename = argv[1];
    }

    FILE *csv = fopen(csv_filename, "w");
    if (!csv) {
        perror("fopen CSV");
        return EXIT_FAILURE;
    }

    // Podés elegir si querés o no header. El enunciado no lo exige,
    // pero ayuda para analizar después en Python/Excel.
    fprintf(csv, "measurement,one_way_delay_seconds\n");
    fflush(csv);

    // Crear socket TCP
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        fclose(csv);
        return EXIT_FAILURE;
    }

    int optval = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval,
                   sizeof(optval)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        close(listen_fd);
        fclose(csv);
        return EXIT_FAILURE;
    }

    // Bind al puerto 20252 en todas las interfaces
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        fclose(csv);
        return EXIT_FAILURE;
    }

    if (listen(listen_fd, 1) < 0) {
        perror("listen");
        close(listen_fd);
        fclose(csv);
        return EXIT_FAILURE;
    }

    printf("Servidor TCP escuchando en puerto %d\n", SERVER_PORT);
    printf("Logueando one-way delay en: %s\n", csv_filename);

    // Aceptar un único cliente (no se exige concurrencia)
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
    if (conn_fd < 0) {
        perror("accept");
        close(listen_fd);
        fclose(csv);
        return EXIT_FAILURE;
    }

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
    printf("Cliente conectado desde %s:%d\n", client_ip,
           ntohs(client_addr.sin_port));

    uint8_t recv_buf[RECV_BUF_SIZE];
    size_t recv_len = 0;
    int measurement_idx = 1;

    while (1) {
        // Leer del socket TCP (pueden llegar PDUs parciales o múltiples juntas)
        ssize_t n = recv(conn_fd, recv_buf + recv_len,
                         sizeof(recv_buf) - recv_len, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;  // reintentar
            }
            perror("recv");
            break;
        }
        if (n == 0) {
            printf("Cliente cerró la conexión.\n");
            break;  // fin normal
        }

        recv_len += (size_t)n;

        // Procesar todas las PDUs completas que haya en el buffer
        while (1) {
            // Buscar el delimitador '|' dentro de recv_buf[0..recv_len-1]
            size_t i;
            int found = 0;
            for (i = 0; i < recv_len; i++) {
                if (recv_buf[i] == '|') {
                    found = 1;
                    break;
                }
            }

            if (!found) {
                // No hay PDU completa aún
                break;
            }

            size_t pdu_len = i + 1;  // posición del '|' + 1

            if (pdu_len < 9) {
                // 8 bytes de timestamp + al menos 1 de payload (en la práctica 500).
                // Si es menor, algo raro pasó: descartamos el primer byte y seguimos.
                fprintf(stderr, "PDU demasiado corta (%zu bytes), descartando un byte\n",
                        pdu_len);
                memmove(recv_buf, recv_buf + 1, recv_len - 1);
                recv_len -= 1;
                continue;
            }

            // Ahora sí: tenemos una PDU completa en recv_buf[0..pdu_len-1].
            // Recién AHORA tomamos el Destination Timestamp (consigna).
            uint64_t dest_ts = get_time_us();

            // Extraer Origin Timestamp de los primeros 8 bytes
            uint64_t origin_ts = 0;
            memcpy(&origin_ts, recv_buf, sizeof(origin_ts));

            // Calcular one-way delay en segundos
            double delay_seconds = 0.0;
            if (dest_ts >= origin_ts) {
                delay_seconds = (double)(dest_ts - origin_ts) / 1000000.0;
            } else {
                // En teoría no debería pasar si los relojes están bien sincronizados,
                // pero por las dudas lo registramos como negativo.
                delay_seconds = - (double)(origin_ts - dest_ts) / 1000000.0;
            }

            // Loguear en CSV: "idx, delay"
            fprintf(csv, "%d, %.5f\n", measurement_idx, delay_seconds);
            fflush(csv);

            printf("Medición %d: delay = %.5f s (origin=%llu, dest=%llu)\n",
                   measurement_idx,
                   delay_seconds,
                   (unsigned long long)origin_ts,
                   (unsigned long long)dest_ts);

            measurement_idx++;

            // Mover el resto de bytes (si quedaron más datos después de la PDU)
            size_t remaining = recv_len - pdu_len;
            if (remaining > 0) {
                memmove(recv_buf, recv_buf + pdu_len, remaining);
            }
            recv_len = remaining;
        }

        // Si llegamos acá, puede que aún queden bytes incompletos en recv_buf
        // que se completarán con próximas lecturas.
    }

    close(conn_fd);
    close(listen_fd);
    fclose(csv);

    printf("Servidor TCP finalizado.\n");
    return EXIT_SUCCESS;
}
