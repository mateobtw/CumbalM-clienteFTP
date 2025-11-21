/* client_ftp.c - Cliente FTP Concurrente (PASV y PORT) */
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
// #include <fcntl.h>

extern int errno;

/* Funciones externas de la librería de sockets (Stevens/Comer style) */
int errexit(const char *format, ...);
int connectTCP(const char *host, const char *service);
int passiveTCP(const char *service, int qlen);
int activo(int s, int *pListenSock, int *pDataSock);

/* * Tamaño de buffer pequeño para simular lentitud junto con usleep.
 * 5MB / 128 bytes = ~40960 paquetes.
 */
#define LINELEN 128

/* Parsea el código de respuesta FTP (ej. "230 Login successful" -> 230) */
int ftpCode(const char *res)
{
    if (!res || strlen(res) < 3)
        return 0;
    /* Conversión simple ASCII a entero de los primeros 3 caracteres */
    return (res[0] - '0') * 100 + (res[1] - '0') * 10 + (res[2] - '0');
}

/* Envía comandos por el canal de control y espera respuesta */
void sendCmd(int s, char *cmd, char *res)
{
    int n;
    size_t len = strlen(cmd);

    /* Protocolo FTP requiere terminación CRLF (\r\n) */
    cmd[len] = '\r';
    cmd[len + 1] = '\n';

    n = write(s, cmd, (int)len + 2);
    if (n < 0)
    {
        perror("Error escribiendo al socket de control");
        return;
    }

    /* Limpiamos el buffer de respuesta */
    memset(res, 0, LINELEN);
    n = read(s, res, LINELEN - 1);
    if (n < 0)
    {
        perror("Error leyendo del socket de control");
        return;
    }
    res[n] = '\0';

    /* Imprimimos la respuesta del servidor (sin el CRLF extra si ya tiene saltos) */
    printf("%s", res);
    /* Aseguramos un salto de línea visual si el server no lo mandó limpio */
    if (res[n - 1] != '\n')
        printf("\n");
}

/* Función auxiliar para entrar en modo PASV y obtener socket de datos */
int pasivo(int s)
{
    int sdata;
    int nport;
    char cmd[128], res[128], *p;
    char host[64], port[8];
    int h1, h2, h3, h4, p1, p2;

    sprintf(cmd, "PASV");
    sendCmd(s, cmd, res);

    /* Buscamos el patrón (h1,h2,h3,h4,p1,p2) en la respuesta */
    p = strchr(res, '(');
    if (!p)
    {
        fprintf(stderr, "Error: Respuesta PASV inesperada: %s\n", res);
        return -1;
    }

    if (sscanf(p + 1, "%d,%d,%d,%d,%d,%d", &h1, &h2, &h3, &h4, &p1, &p2) != 6)
    {
        fprintf(stderr, "Error: No se pudo parsear la IP/Puerto de PASV\n");
        return -1;
    }

    snprintf(host, sizeof(host), "%d.%d.%d.%d", h1, h2, h3, h4);
    nport = p1 * 256 + p2; /* Cálculo del puerto: p1*256 + p2 */
    snprintf(port, sizeof(port), "%d", nport);

    /* Conectamos al canal de datos */
    sdata = connectTCP(host, port);
    if (sdata < 0)
    {
        perror("Error conectando al canal de datos (PASV)");
    }

    return sdata;
}

/* Función auxiliar para modo activo (PORT):
 * - Crea un socket de escucha local en puerto efímero.
 * - Construye y envía el comando PORT al servidor usando la IP/puerto local.
 * - No hace accept(): deja el socket de escucha en *pListenSock.
 *   El accept() se realiza después, cuando el servidor abre el canal de datos.
 * Devuelve 0 en éxito, -1 en error.
 */
int activo(int s, int *pListenSock, int *pDataSock)
{
    int lsock;
    struct sockaddr_in sin, addrSvr;
    socklen_t slen = sizeof(sin), alen = sizeof(addrSvr);
    char ip[64];
    int p1, p2;
    char cmd[128], res[128];

    *pListenSock = -1;
    *pDataSock = -1;

    /* Crear socket, bind a puerto 0 para pedir puerto efímero */
    lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock < 0)
    {
        perror("[activo] socket");
        return -1;
    }

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = htons(0); /* puerto efímero */

    if (bind(lsock, (struct sockaddr *)&sin, sizeof(sin)) < 0)
    {
        perror("[activo] bind");
        close(lsock);
        return -1;
    }

    if (listen(lsock, 5) < 0)
    {
        perror("[activo] listen");
        close(lsock);
        return -1;
    }

    if (getsockname(lsock, (struct sockaddr *)&sin, &slen) < 0)
    {
        perror("[activo] getsockname");
        close(lsock);
        return -1;
    }

    /* Obtener mi IP basada en el socket de control */
    if (getsockname(s, (struct sockaddr *)&addrSvr, &alen) < 0)
    {
        perror("[activo] getsockname control");
        close(lsock);
        return -1;
    }

    /* Obtener IP local donde quedó vinculado el socket */
    {
        const char *raw_ip = inet_ntoa(addrSvr.sin_addr);
        strncpy(ip, raw_ip, sizeof(ip) - 1);
        ip[sizeof(ip) - 1] = '\0';
        for (size_t i = 0; ip[i] != '\0'; i++)
        {
            if (ip[i] == '.')
                ip[i] = ',';
        }
    }

    int port = ntohs(sin.sin_port);
    p1 = port / 256;
    p2 = port % 256;

    snprintf(cmd, sizeof(cmd), "PORT %s,%d,%d", ip, p1, p2);
    sendCmd(s, cmd, res);
    if (ftpCode(res) >= 500)
    {
        fprintf(stderr, "[activo] Servidor rechazo PORT: %s", res);
        close(lsock);
        return -1;
    }

    /* No hacemos accept aquí: lo hará el hijo cuando corresponda. */
    *pListenSock = lsock;
    *pDataSock = -1;

    return 0;
}

void ayuda()
{
    printf("\n--- Cliente FTP Concurrente ---\n");
    printf("Comandos disponibles:\n");
    printf("  HELP               - Muestra este menu\n");
    printf("  MODE PASV|PORT     - Selecciona modo de datos (pasivo/activo)\n");
    printf("  LIST               - Lista archivos del servidor\n");
    printf("  PWD                - Muestra directorio actual remoto\n");
    printf("  CWD <dir>          - Cambia de directorio remoto\n");
    printf("  MKD <dir>          - Crea un directorio remoto\n");
    printf("  RMD <dir>          - Elimina un directorio remoto\n");
    printf("  DELE <archivo>     - Elimina un archivo remoto\n");
    printf("  RETR <archivo>     - Descarga archivo (Concurrente/Background)\n");
    printf("  STOR <archivo>     - Sube archivo (Concurrente/Background)\n");
    printf("  QUIT               - Salir\n\n");
}

/* * Proceso Hijo: Descarga (RETR)
 * Crea su propia conexión de control para no interferir con el padre.
 */
void do_retr_child(const char *host, const char *service,
                   const char *user, const char *password,
                   const char *filename,
                   int usePortMode)
{
    int s, sdata, n;
    char cmd[128], res[128];
    char data[LINELEN + 1];
    FILE *fp;

    printf("[Hijo %d] Iniciando descarga de %s...\n", getpid(), filename);

    s = connectTCP(host, service);
    if (s < 0)
    {
        perror("[Hijo] connectTCP control");
        exit(1);
    }

    /* Consumir banner de bienvenida */
    read(s, res, LINELEN);

    /* Login */
    sprintf(cmd, "USER %s", user);
    sendCmd(s, cmd, res);
    sprintf(cmd, "PASS %s", password);
    sendCmd(s, cmd, res);

    if (ftpCode(res) != 230)
    {
        fprintf(stderr, "[Hijo %d] Error de autenticación.\n", getpid());
        close(s);
        exit(1);
    }

    /* Seleccionar modo de datos: PASV (0) o PORT (1) */
    int listenSock = -1;
    sdata = -1;
    if (!usePortMode)
    {
        /* Modo PASV: el servidor escucha y el cliente se conecta */
        sdata = pasivo(s);
        if (sdata < 0)
        {
            close(s);
            exit(1);
        }
    }
    else
    {
        /* Modo PORT: el cliente escucha y el servidor se conecta */
        if (activo(s, &listenSock, &sdata) < 0)
        {
            close(s);
            exit(1);
        }
    }

    sprintf(cmd, "RETR %s", filename);
    sendCmd(s, cmd, res);

    if (ftpCode(res) >= 500)
    {
        fprintf(stderr, "[Hijo %d] Servidor rechazó RETR: %s", getpid(), res);
        if (sdata >= 0)
            close(sdata);
        if (listenSock >= 0)
            close(listenSock);
        close(s);
        exit(1);
    }

    fp = fopen(filename, "wb");
    if (!fp)
    {
        perror("[Hijo] Error creando archivo local");
        if (sdata >= 0)
            close(sdata);
        if (listenSock >= 0)
            close(listenSock);
        close(s);
        exit(1);
    }

    /* Bucle de lectura con retardo artificial */
    int total_bytes = 0;
    if (usePortMode && sdata < 0)
    {
        struct sockaddr_in addrSvr;
        socklen_t alen = sizeof(addrSvr);
        sdata = accept(listenSock, (struct sockaddr *)&addrSvr, &alen);
        if (sdata < 0)
        {
            perror("[Hijo] accept PORT");
            fclose(fp);
            close(listenSock);
            close(s);
            exit(1);
        }
        close(listenSock);
        listenSock = -1;
    }

    while ((n = recv(sdata, data, LINELEN, 0)) > 0)
    {
        fwrite(data, 1, n, fp);
        total_bytes += n;
        /* Retardo para 5MB en ~30s usando buffer de 128 bytes */
        usleep(732);
    }

    fclose(fp);
    close(sdata);

    /* Leer respuesta final del servidor (ej. "226 Transfer complete") */
    n = read(s, res, LINELEN);
    if (n > 0)
    {
        res[n] = '\0';
        printf("[Hijo %d] %s", getpid(), res);
    }

    close(s);
    printf("[Hijo %d] Finalizado: %s (%d bytes descargados)\n", getpid(), filename, total_bytes);
    exit(0);
}

/* * Proceso Hijo: Subida (STOR)
 */
void do_stor_child(const char *host, const char *service,
                   const char *user, const char *password,
                   const char *filename,
                   int usePortMode)
{
    int s, sdata, n;
    char cmd[128], res[128];
    char data[LINELEN + 1];
    FILE *fp;

    fp = fopen(filename, "rb");
    if (!fp)
    {
        perror("[Hijo] No se puede leer archivo local");
        exit(1);
    }

    printf("[Hijo %d] Iniciando subida de %s...\n", getpid(), filename);

    s = connectTCP(host, service);
    if (s < 0)
    {
        perror("[Hijo] connectTCP control");
        fclose(fp);
        exit(1);
    }

    read(s, res, LINELEN); // Banner

    /* Login */
    sprintf(cmd, "USER %s", user);
    sendCmd(s, cmd, res);
    sprintf(cmd, "PASS %s", password);
    sendCmd(s, cmd, res);

    if (ftpCode(res) != 230)
    {
        fprintf(stderr, "[Hijo %d] Error login.\n", getpid());
        fclose(fp);
        close(s);
        exit(1);
    }

    /* Seleccionar modo de datos: PASV (0) o PORT (1) */
    int listenSock = -1;
    sdata = -1;
    if (!usePortMode)
    {
        sdata = pasivo(s);
        if (sdata < 0)
        {
            fclose(fp);
            close(s);
            exit(1);
        }
    }
    else
    {
        if (activo(s, &listenSock, &sdata) < 0)
        {
            fclose(fp);
            close(s);
            exit(1);
        }
    }

    sprintf(cmd, "STOR %s", filename);
    sendCmd(s, cmd, res);

    if (ftpCode(res) >= 500)
    {
        fprintf(stderr, "[Hijo %d] Error STOR: %s", getpid(), res);
        fclose(fp);
        if (sdata >= 0)
            close(sdata);
        if (listenSock >= 0)
            close(listenSock);
        close(s);
        exit(1);
    }

    int total_bytes = 0;
    if (usePortMode && sdata < 0)
    {
        struct sockaddr_in addrSvr;
        socklen_t alen = sizeof(addrSvr);
        sdata = accept(listenSock, (struct sockaddr *)&addrSvr, &alen);
        if (sdata < 0)
        {
            perror("[Hijo] accept PORT");
            close(listenSock);
            fclose(fp);
            close(s);
            exit(1);
        }
        close(listenSock);
        listenSock = -1;
    }

    while ((n = fread(data, 1, LINELEN, fp)) > 0)
    {
        if (send(sdata, data, n, 0) < 0)
        {
            perror("[Hijo] Error enviando datos");
            break;
        }
        total_bytes += n;
        usleep(732); /* Retardo artificial */
    }

    fclose(fp);
    close(sdata);

    n = read(s, res, LINELEN);
    if (n > 0)
    {
        res[n] = '\0';
        printf("[Hijo %d] %s", getpid(), res);
    }

    close(s);
    printf("[Hijo %d] Subida terminada: %s (%d bytes enviados)\n", getpid(), filename, total_bytes);
    exit(0);
}

int main(int argc, char *argv[])
{
    char *host = "localhost";
    char *service = "ftp";
    char cmd[128], res[128];
    char data[LINELEN + 1];
    char user[32], password[64], *pass;
    char prompt[64], *ucmd, *arg;
    int s, sdata, n;
    int usePortMode = 0; /* 0 = PASV (por defecto), 1 = PORT */

    if (argc > 1)
        host = argv[1];
    if (argc > 2)
        service = argv[2];

    /* --- Conexión Inicial del Padre --- */
    s = connectTCP(host, service);
    if (s < 0)
    {
        fprintf(stderr, "No se pudo conectar a %s:%s\n", host, service);
        exit(1);
    }

    /* Banner */
    n = read(s, res, LINELEN);
    res[n] = '\0';
    printf("%s", res);

    /* Login Interactivo */
    while (1)
    {
        printf("Usuario: ");
        scanf("%31s", user);

        sprintf(cmd, "USER %s", user);
        sendCmd(s, cmd, res);

        pass = getpass("Contraseña: ");
        strncpy(password, pass, sizeof(password) - 1);
        password[sizeof(password) - 1] = '\0';

        sprintf(cmd, "PASS %s", password);
        sendCmd(s, cmd, res);

        if (ftpCode(res) == 230)
            break;
        printf("Login fallido, intente nuevamente.\n");
    }

    /* Limpieza de buffer de entrada */
    fgets(prompt, sizeof(prompt), stdin);

    ayuda();

    while (1)
    {
        /* Limpiar procesos zombies de transferencias terminadas */
        while (waitpid(-1, NULL, WNOHANG) > 0)
            ;

        printf("ftp> ");
        if (fgets(prompt, sizeof(prompt), stdin) == NULL)
            break;

        prompt[strcspn(prompt, "\n")] = 0; // Quitar \n
        if (strlen(prompt) == 0)
            continue;

        ucmd = strtok(prompt, " ");

        if (!ucmd)
            continue;

        /* --- COMANDOS LOCALES O SIMPLES --- */

        if (strcmp(ucmd, "QUIT") == 0)
        {
            sprintf(cmd, "QUIT");
            sendCmd(s, cmd, res);
            close(s);
            exit(0);
        }
        else if (strcmp(ucmd, "HELP") == 0)
        {
            ayuda();
        }
        else if (strcmp(ucmd, "MODE") == 0)
        {
            arg = strtok(NULL, " ");
            if (!arg)
            {
                printf("Modo actual de datos: %s\n", usePortMode ? "PORT (activo)" : "PASV (pasivo)");
            }
            else if (strcasecmp(arg, "PASV") == 0)
            {
                usePortMode = 0;
                printf("[Info] Modo de datos cambiado a PASV (pasivo).\n");
            }
            else if (strcasecmp(arg, "PORT") == 0)
            {
                usePortMode = 1;
                printf("[Info] Modo de datos cambiado a PORT (activo).\n");
            }
            else
            {
                printf("Uso: MODE PASV | MODE PORT\n");
            }
        }

        /* --- COMANDOS DE GESTIÓN (Sin datos, solo control) --- */

        else if (strcmp(ucmd, "PWD") == 0)
        {
            sprintf(cmd, "PWD");
            sendCmd(s, cmd, res);
        }
        else if (strcmp(ucmd, "CWD") == 0)
        {
            arg = strtok(NULL, " ");
            if (arg)
            {
                sprintf(cmd, "CWD %s", arg);
                sendCmd(s, cmd, res);
            }
            else
                printf("Uso: CWD <directorio>\n");
        }
        else if (strcmp(ucmd, "MKD") == 0)
        {
            arg = strtok(NULL, " ");
            if (arg)
            {
                sprintf(cmd, "MKD %s", arg);
                sendCmd(s, cmd, res);
            }
            else
                printf("Uso: MKD <directorio>\n");
        }
        else if (strcmp(ucmd, "RMD") == 0)
        {
            arg = strtok(NULL, " ");
            if (arg)
            {
                sprintf(cmd, "RMD %s", arg);
                sendCmd(s, cmd, res);
            }
            else
                printf("Uso: RMD <directorio>\n");
        }
        else if (strcmp(ucmd, "DELE") == 0)
        {
            arg = strtok(NULL, " ");
            if (arg)
            {
                sprintf(cmd, "DELE %s", arg);
                sendCmd(s, cmd, res);
            }
            else
                printf("Uso: DELE <archivo>\n");
        }

        /* --- COMANDOS CON TRANSFERENCIA DE DATOS --- */

        else if (strcmp(ucmd, "LIST") == 0)
        {
            /* LIST lo dejamos bloqueante/síncrono en el padre para ver el output directo */
            sdata = pasivo(s);
            if (sdata >= 0)
            {
                sprintf(cmd, "LIST");
                sendCmd(s, cmd, res); // 150 Opening mode...

                while ((n = recv(sdata, data, LINELEN, 0)) > 0)
                {
                    fwrite(data, 1, n, stdout);
                }
                close(sdata);

                /* Leer respuesta final (226 Transfer complete) */
                memset(res, 0, LINELEN);
                if (read(s, res, LINELEN) > 0)
                    printf("%s", res);
            }
        }

        /* --- COMANDOS CONCURRENTES (FORK) --- */

        else if (strcmp(ucmd, "RETR") == 0)
        {
            arg = strtok(NULL, " ");
            if (!arg)
            {
                printf("Uso: RETR <archivo>\n");
                continue;
            }

            pid_t pid = fork();
            if (pid == 0)
            {
                /* HIJO */
                close(s); // El hijo cierra el socket del padre
                do_retr_child(host, service, user, password, arg, usePortMode);
            }
            else if (pid > 0)
            {
                /* PADRE */
                printf("[Info] Descarga iniciada en proceso PID=%d\n", pid);
            }
            else
            {
                perror("Error en fork");
            }
        }
        else if (strcmp(ucmd, "STOR") == 0)
        {
            arg = strtok(NULL, " ");
            if (!arg)
            {
                printf("Uso: STOR <archivo>\n");
                continue;
            }

            pid_t pid = fork();
            if (pid == 0)
            {
                /* HIJO */
                close(s);
                do_stor_child(host, service, user, password, arg, usePortMode);
            }
            else if (pid > 0)
            {
                /* PADRE */
                printf("[Info] Subida iniciada en proceso PID=%d\n", pid);
            }
            else
            {
                perror("Error en fork");
            }
        }
        else
        {
            printf("Comando desconocido: %s. Use HELP para ver la lista de comandos.\n", ucmd);
        }
    }
    return 0;
}
