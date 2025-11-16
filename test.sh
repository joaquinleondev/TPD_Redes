#!/bin/bash

# Colores para output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=== Pruebas UDP Stop & Wait ==="

# Limpiar artefactos previos
rm -rf uploads
rm -f *.log test.bin empty.bin c1.bin c2.bin c3.bin

# Crear archivo de prueba de 20 kB
echo -e "${YELLOW}Creando archivo de prueba (20 kB)...${NC}"
dd if=/dev/zero of=test.bin bs=1024 count=20 2>/dev/null

# Archivo vacío para probar transferencia de tamaño 0
touch empty.bin

# Verificar compilación
if [ ! -f "client" ] || [ ! -f "server" ]; then
    echo -e "${RED}Error: compilar primero con 'make'${NC}"
    exit 1
fi

# Iniciar servidor en background
echo -e "${YELLOW}Iniciando servidor...${NC}"
./server credentials.txt > server.log 2>&1 &
SERVER_PID=$!
sleep 2

# Verificar que el servidor está corriendo
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo -e "${RED}Error: el servidor no se inició correctamente${NC}"
    cat server.log
    exit 1
fi

echo -e "${GREEN}Servidor iniciado (PID: $SERVER_PID)${NC}"

########################################################################
# Prueba 1: Transferencia exitosa completa (4 fases, archivo no vacío)
########################################################################
echo -e "\n${YELLOW}Prueba 1: Transferencia exitosa (archivo no vacío)${NC}"
./client 127.0.0.1 test.bin test_credential > client_basic.log 2>&1

if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ Transferencia completada${NC}"

    # Verificar archivo recibido
    if [ -f "uploads/test.bin" ]; then
        echo -e "${GREEN}✓ Archivo creado en servidor${NC}"

        # Comparar checksums
        CLIENT_MD5=$(md5sum test.bin | awk '{print $1}')
        SERVER_MD5=$(md5sum uploads/test.bin | awk '{print $1}')

        if [ "$CLIENT_MD5" == "$SERVER_MD5" ]; then
            echo -e "${GREEN}✓ Checksums coinciden (integridad OK)${NC}"
        else
            echo -e "${RED}✗ Checksums NO coinciden${NC}"
        fi
    else
        echo -e "${RED}✗ Archivo no encontrado en servidor${NC}"
    fi
else
    echo -e "${RED}✗ Error en transferencia${NC}"
    cat client_basic.log
fi

########################################################################
# Prueba 2: Credenciales inválidas (HELLO debe fallar)
########################################################################
echo -e "\n${YELLOW}Prueba 2: Credenciales inválidas${NC}"
./client 127.0.0.1 test.bin credencial_invalida > client_bad_cred.log 2>&1

if [ $? -ne 0 ]; then
    echo -e "${GREEN}✓ Rechazo esperado de credenciales inválidas${NC}"
else
    echo -e "${RED}✗ No se rechazaron credenciales inválidas${NC}"
fi

########################################################################
# Prueba 3: Filename inválido (muy corto)
########################################################################
echo -e "\n${YELLOW}Prueba 3: Filename inválido (muy corto)${NC}"
./client 127.0.0.1 abc test_credential > client_short_name.log 2>&1

if [ $? -ne 0 ]; then
    echo -e "${GREEN}✓ Rechazo esperado de filename corto${NC}"
else
    echo -e "${RED}✗ No se rechazó filename corto${NC}"
fi

########################################################################
# Prueba 4: Transferencia de archivo vacío
########################################################################
echo -e "\n${YELLOW}Prueba 4: Transferencia de archivo vacío${NC}"
./client 127.0.0.1 empty.bin test_credential > client_empty.log 2>&1

if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ Transferencia de archivo vacío completada${NC}"
    if [ -f "uploads/empty.bin" ]; then
        SIZE=$(stat -c%s "uploads/empty.bin" 2>/dev/null || echo 0)
        if [ "$SIZE" -eq 0 ]; then
            echo -e "${GREEN}✓ Archivo vacío recibido correctamente${NC}"
        else
            echo -e "${RED}✗ Archivo en servidor no es de tamaño 0${NC}"
        fi
    else
        echo -e "${RED}✗ Archivo vacío no encontrado en servidor${NC}"
    fi
else
    echo -e "${RED}✗ Error en transferencia de archivo vacío${NC}"
    cat client_empty.log
fi

########################################################################
# Prueba 5: Concurrencia básica de múltiples clientes
########################################################################
echo -e "\n${YELLOW}Prueba 5: Concurrencia de múltiples clientes${NC}"

cp test.bin c1.bin
cp test.bin c2.bin
cp test.bin c3.bin

./client 127.0.0.1 c1.bin test_credential > client_c1.log 2>&1 &
PID_C1=$!
./client 127.0.0.1 c2.bin test_credential > client_c2.log 2>&1 &
PID_C2=$!
./client 127.0.0.1 c3.bin test_credential > client_c3.log 2>&1 &
PID_C3=$!

wait "$PID_C1"
RET_C1=$?
wait "$PID_C2"
RET_C2=$?
wait "$PID_C3"
RET_C3=$?

if [ $RET_C1 -eq 0 ] && [ $RET_C2 -eq 0 ] && [ $RET_C3 -eq 0 ]; then
    echo -e "${GREEN}✓ Los 3 clientes finalizaron correctamente${NC}"
    OK_MD5=true
    for F in c1.bin c2.bin c3.bin; do
        if [ ! -f "uploads/$F" ]; then
            echo -e "${RED}✗ uploads/$F no fue creado${NC}"
            OK_MD5=false
        else
            LOCAL_MD5=$(md5sum "$F" | awk '{print $1}')
            REMOTE_MD5=$(md5sum "uploads/$F" | awk '{print $1}')
            if [ "$LOCAL_MD5" != "$REMOTE_MD5" ]; then
                echo -e "${RED}✗ Checksums NO coinciden para $F${NC}"
                OK_MD5=false
            fi
        fi
    done
    if $OK_MD5; then
        echo -e "${GREEN}✓ Concurrencia básica verificada (3 clientes)${NC}"
    fi
else
    echo -e "${RED}✗ Alguno de los clientes concurrentes falló${NC}"
fi

########################################################################
# Pruebas 6+: Validación de fases y de Seq Num vía PDUs manuales (python3)
########################################################################

if command -v python3 >/dev/null 2>&1; then
    echo -e "\n${YELLOW}Prueba 6: WRQ sin HELLO previo (violación de fases)${NC}"
    python3 - << 'PY'
import socket

SERVER_IP = "127.0.0.1"
PORT = 20252

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
# WRQ (Type=2) con Seq=1 y filename "nofase\0" sin haber enviado HELLO
pdu = bytes([2, 1]) + b"nofase\x00"
s.sendto(pdu, (SERVER_IP, PORT))
s.close()
PY

    sleep 1
    if grep -q "WRQ en estado incorrecto" server.log; then
        echo -e "${GREEN}✓ WRQ sin autenticación previa descartado correctamente${NC}"
    else
        echo -e "${RED}✗ No se registró WRQ en estado incorrecto en server.log${NC}"
    fi

    echo -e "\n${YELLOW}Prueba 7: DATA sin WRQ previo (violación de fases)${NC}"
    python3 - << 'PY'
import socket

SERVER_IP = "127.0.0.1"
PORT = 20252

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
# DATA (Type=3) con Seq=0 y algunos bytes sin WRQ previo
pdu = bytes([3, 0]) + b"abcd"
s.sendto(pdu, (SERVER_IP, PORT))
s.close()
PY

    sleep 1
    if grep -q "DATA sin WRQ previo" server.log; then
        echo -e "${GREEN}✓ DATA sin WRQ previo descartado correctamente${NC}"
    else
        echo -e "${RED}✗ No se registró DATA sin WRQ previo en server.log${NC}"
    fi

    echo -e "\n${YELLOW}Prueba 8: FIN sin transferencia previa (violación de fases)${NC}"
    python3 - << 'PY'
import socket

SERVER_IP = "127.0.0.1"
PORT = 20252

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
# FIN (Type=5) con Seq=0 y nombre 'finbad\0' sin haber enviado DATA
pdu = bytes([5, 0]) + b"finbad\x00"
s.sendto(pdu, (SERVER_IP, PORT))
s.close()
PY

    sleep 1
    if grep -q "FIN en estado incorrecto" server.log; then
        echo -e "${GREEN}✓ FIN sin transferencia previa descartado correctamente${NC}"
    else
        echo -e "${RED}✗ No se registró FIN en estado incorrecto en server.log${NC}"
    fi

    echo -e "\n${YELLOW}Prueba 9: Seq Num incorrecto (violación de Stop & Wait)${NC}"
    python3 - << 'PY'
import socket, time

SERVER_IP = "127.0.0.1"
PORT = 20252

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

# HELLO válido
pdu = bytes([1, 0]) + b"test_credential"
s.sendto(pdu, (SERVER_IP, PORT))
time.sleep(0.2)

# WRQ válido con filename 'pyseq\0'
pdu = bytes([2, 1]) + b"pyseq\x00"
s.sendto(pdu, (SERVER_IP, PORT))
time.sleep(0.2)

# DATA con Seq=1 cuando el servidor espera Seq=0
pdu = bytes([3, 1]) + b"BBBB"
s.sendto(pdu, (SERVER_IP, PORT))

s.close()
PY

    sleep 1
    if grep -q "Seq num incorrecto:" server.log; then
        echo -e "${GREEN}✓ Seq Num incorrecto detectado y registrado por el servidor${NC}"
    else
        echo -e "${RED}✗ No se registró Seq Num incorrecto en server.log${NC}"
    fi
else
    echo -e "\n${YELLOW}python3 no disponible: se omiten pruebas de PDUs manuales (6-9)${NC}"
fi

########################################################################
# Cleanup
########################################################################
echo -e "\n${YELLOW}Limpiando...${NC}"
kill "$SERVER_PID" 2>/dev/null
wait "$SERVER_PID" 2>/dev/null

rm -f test.bin empty.bin c1.bin c2.bin c3.bin
rm -f client_basic.log client_bad_cred.log client_short_name.log client_empty.log
rm -f client_c1.log client_c2.log client_c3.log

echo -e "\n${GREEN}=== Pruebas completadas ===${NC}"