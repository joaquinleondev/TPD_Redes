#!/bin/bash

# Colores para output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=== Pruebas TCP One-Way Delay ==="

# Limpiar artefactos previos
rm -f one_way_delay_*.csv tcp_server_*.log

# Verificar compilación
if [ ! -x "tcp_client" ] || [ ! -x "tcp_server" ]; then
    echo -e "${RED}Error: compilar primero con 'make tcp_client tcp_server' o 'make all'${NC}"
    exit 1
fi

start_server() {
    local csv_file="$1"
    local log_file="$2"

    echo -e "${YELLOW}Iniciando tcp_server -> CSV: ${csv_file}${NC}"
    ./tcp_server "${csv_file}" > "${log_file}" 2>&1 &
    SERVER_PID=$!
    sleep 1

    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo -e "${RED}Error: tcp_server no se inició correctamente${NC}"
        echo "Log de tcp_server:"
        cat "${log_file}"
        exit 1
    fi
}

wait_server() {
    if [ -n "${SERVER_PID:-}" ]; then
        wait "$SERVER_PID" 2>/dev/null
        SERVER_PID=""
    fi
}

########################################################################
# Prueba 1: corrida básica con CSV generado
########################################################################
echo -e "\n${YELLOW}Prueba 1: corrida básica (d=50 ms, N=3 s)${NC}"

CSV1="one_way_delay_p1.csv"
LOG1="tcp_server_p1.log"

start_server "$CSV1" "$LOG1"

echo -e "${YELLOW}Ejecutando tcp_client...${NC}"
./tcp_client 127.0.0.1 -d 50 -N 3
RET_CLIENT=$?

wait_server

if [ $RET_CLIENT -ne 0 ]; then
    echo -e "${RED}✗ tcp_client devolvió código $RET_CLIENT en Prueba 1${NC}"
    exit 1
fi

if [ ! -f "$CSV1" ]; then
    echo -e "${RED}✗ No se generó el archivo CSV ${CSV1}${NC}"
    exit 1
fi

LINES=$(wc -l < "$CSV1")
MEAS=$((LINES - 1)) # restar header

if [ "$MEAS" -le 0 ]; then
    echo -e "${RED}✗ CSV no contiene mediciones (solo header)${NC}"
    exit 1
fi

echo -e "${GREEN}✓ CSV generado con ${MEAS} mediciones en Prueba 1${NC}"

########################################################################
# Prueba 2: más frecuencia de envío => más mediciones
########################################################################
echo -e "\n${YELLOW}Prueba 2: comparación de densidad (d=50 ms vs d=10 ms, N=2 s)${NC}"

CSV_SLOW="one_way_delay_slow.csv"
CSV_FAST="one_way_delay_fast.csv"
LOG_SLOW="tcp_server_slow.log"
LOG_FAST="tcp_server_fast.log"

# Caso "lento"
start_server "$CSV_SLOW" "$LOG_SLOW"
./tcp_client 127.0.0.1 -d 50 -N 2
RET_SLOW=$?
wait_server

if [ $RET_SLOW -ne 0 ]; then
    echo -e "${RED}✗ tcp_client falló en modo lento (Prueba 2)${NC}"
    exit 1
fi

# Caso "rápido"
start_server "$CSV_FAST" "$LOG_FAST"
./tcp_client 127.0.0.1 -d 10 -N 2
RET_FAST=$?
wait_server

if [ $RET_FAST -ne 0 ]; then
    echo -e "${RED}✗ tcp_client falló en modo rápido (Prueba 2)${NC}"
    exit 1
fi

if [ ! -f "$CSV_SLOW" ] || [ ! -f "$CSV_FAST" ]; then
    echo -e "${RED}✗ No se generaron uno o ambos CSV en Prueba 2${NC}"
    exit 1
fi

MEAS_SLOW=$(( $(wc -l < "$CSV_SLOW") - 1 ))
MEAS_FAST=$(( $(wc -l < "$CSV_FAST") - 1 ))

echo -e "${YELLOW}Mediciones modo lento:  ${MEAS_SLOW}${NC}"
echo -e "${YELLOW}Mediciones modo rápido: ${MEAS_FAST}${NC}"

if [ "$MEAS_FAST" -le "$MEAS_SLOW" ]; then
    echo -e "${RED}✗ No se observó mayor cantidad de mediciones en d=10 ms que en d=50 ms${NC}"
    exit 1
fi

echo -e "${GREEN}✓ d más chico produce más mediciones (como se espera)${NC}"

########################################################################
# Prueba 3: formato del CSV y delays no negativos
########################################################################
echo -e "\n${YELLOW}Prueba 3: formato CSV y delays no negativos (usando ${CSV1})${NC}"

HEADER=$(head -n 1 "$CSV1")
if [ "$HEADER" != "measurement,one_way_delay_seconds" ]; then
    echo -e "${RED}✗ Header del CSV inesperado: '${HEADER}'${NC}"
    exit 1
fi

# Verificar que todas las líneas (excepto header) tienen dos campos separados por coma
awk -F, 'NR>1 { if (NF != 2) { bad=1 } } END { exit bad }' "$CSV1"
if [ $? -ne 0 ]; then
    echo -e "${RED}✗ Formato de líneas en CSV inválido (se esperaban 2 columnas)${NC}"
    exit 1
fi

# Verificar que los delays sean numéricos y no negativos
awk -F, 'NR>1 {
    gsub(/^[ \t]+|[ \t]+$/, "", $2);
    if ($2 !~ /^-?[0-9]*\.?[0-9]+$/) { bad=1 }
    if ($2 < 0) { bad=1 }
} END { exit bad }' "$CSV1"

if [ $? -ne 0 ]; then
    echo -e "${RED}✗ Se encontraron valores de delay inválidos o negativos en el CSV${NC}"
    exit 1
fi

echo -e "${GREEN}✓ Formato CSV correcto y delays no negativos en ${CSV1}${NC}"

echo -e "\n${GREEN}=== Pruebas TCP One-Way Delay completadas exitosamente ===${NC}"
