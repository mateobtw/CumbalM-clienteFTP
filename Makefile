# Makefile para Cliente FTP Concurrente
# Autor: CumbalM

CC = gcc
CFLAGS = -Wall
TARGET = CumbalM-clienteFTP
SOURCES = CumbalM-clienteFTP.c connectTCP.c connectsock.c errexit.c
OBJS = $(SOURCES:.c=.o)

# Regla principal
all: $(TARGET) test_files

# Compilación del ejecutable
$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCES)

# Regla utilitaria para crear archivos de prueba de 5MB
# Crea 'archivo_cliente.bin' (para subir) y 'archivo_server.bin' (para descargar)
# NOTA: 'archivo_server.bin' debe moverse manualmente al HOME del usuario FTP si no es el mismo directorio.
test_files:
	dd if=/dev/urandom of=archivo_cliente.bin bs=1M count=5
	dd if=/dev/urandom of=archivo_server.bin bs=1M count=5
	@echo "--- Archivos de prueba de 5MB generados ---"
	@echo "IMPORTANTE: Mueve 'archivo_server.bin' a la carpeta del servidor FTP (ej: /home/tu_usuario/) antes de probar."

clean:
	rm -f $(TARGET) *.o archivo_cliente.bin archivo_server.bin

.PHONY: all clean test_files
