Cliente FTP Concurrente (Linux)

Autor: Mateobt (CumbalM)
Asignatura: Sistemas Distribuidos
Archivo Principal: CumbalM-clienteFTP.c

Este proyecto implementa un cliente FTP funcional en lenguaje C, diseñado bajo el estándar RFC 959. Su arquitectura permite manejar conexiones de control y datos separadas, destacando por su capacidad de concurrencia: permite subir (STOR) y descargar (RETR) archivos en segundo plano sin bloquear la terminal del usuario.

Características Técnicas

Arquitectura: Modelo Cliente-Servidor con sockets TCP y manejo de procesos (fork).

Modos de Datos: Soporte completo para modo Pasivo (PASV) y Activo (PORT).

Concurrencia Real:

Cada transferencia de archivos se ejecuta en un proceso hijo independiente.

El proceso hijo establece una nueva conexión de control, se re-autentica automáticamente y negocia su propio canal de datos.

Simulación para Pruebas:

Se utiliza un buffer reducido de 128 bytes y un retardo artificial (usleep) en el bucle de transferencia.

Objetivo: Esto permite visualizar la concurrencia y el estado de los procesos en ps aux incluso en conexiones locales rápidas (localhost) con archivos de 5MB.

Gestión de Recursos: Implementación de waitpid con WNOHANG para la limpieza no bloqueante de procesos zombies.

Requisitos Previos

Compilador gcc.

Servidor FTP local (recomendado: vsftpd).

Sistema Operativo Linux.

Configuración del Servidor (vsftpd)

Para que el cliente funcione correctamente (especialmente la escritura de archivos y la concurrencia), debe configurar el servidor local.

Edite el archivo de configuración:

sudo nano /etc/vsftpd.conf


Asegúrese de tener estas líneas descomentadas o agregadas:

anonymous_enable=NO
local_enable=YES
write_enable=YES           # Indispensable para STOR, MKD, RMD
# chroot_local_user=NO     # Opcional: permite navegar fuera del home


Guarde los cambios y reinicie el servicio:

sudo systemctl restart vsftpd

Compilación y Preparación

El proyecto incluye un Makefile automatizado que compila el código y genera los archivos binarios de prueba (5MB) en un solo paso.

Compilar y generar archivos de prueba:

make


Esto creará:

CumbalM-clienteFTP

archivo_cliente.bin

archivo_server.bin

Mover el archivo de prueba al servidor:

mv archivo_server.bin ~/

Secuencia de Prueba y Demostración

Siga estos pasos estrictamente para evidenciar la concurrencia frente al profesor.

1. Ejecución y Comandos Básicos

Inicie el cliente:

./CumbalM-clienteFTP localhost
# Ingrese su usuario y contraseña FTP


Pruebe comandos básicos:

ftp> PWD
ftp> MKD test_folder
ftp> LIST
ftp> CWD test_folder
ftp> PWD
ftp> CWD ..
ftp> RMD test_folder

2. Prueba de Concurrencia (PASO CRÍTICO)

Lanzaremos una subida y una descarga simultáneas usando modos opuestos.

1) En el cliente FTP:
ftp> MODE PASV
ftp> STOR archivo_cliente.bin
# “Subida iniciada en proceso PID=...”

ftp> MODE PORT
ftp> RETR archivo_server.bin
# “Descarga iniciada en proceso PID=...”

2) En OTRA terminal:

Ejecute:

ps aux | grep CumbalM


Debe ver 3 procesos activos:

El padre (control de terminal).

Hijo subiendo.

Hijo descargando.

3) Espere en el cliente FTP

Ambas transferencias terminarán con “226 Transfer complete”.

4) Verificación de Zombies

En la otra terminal:

ps aux | grep CumbalM


Debe ver procesos con estado Z o [defunct].

5) Limpieza de Zombies

En el cliente FTP simplemente presione ENTER o ejecute:

ftp> LIST


Esto activa el waitpid(WNOHANG) y limpia los zombies.

6) Verificación Final

En otra terminal:

ps aux | grep CumbalM


Solo debe quedar el proceso padre.

3. Finalización

Elimine los archivos creados y salga:

ftp> DELE archivo_server.bin
ftp> QUIT
