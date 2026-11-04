#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <orbis/libkernel.h>

static FILE *f;
#define LOG(...) do { fprintf(f, __VA_ARGS__); fflush(f); } while (0)

static int conectar(int puerto, int espera) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        LOG("puerto %d: no pude crear socket (errno=%d)\n", puerto, errno);
        return -1;
    }
    struct timeval tv;
    tv.tv_sec = espera;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(puerto);
    a.sin_addr.s_addr = htonl(0x7F000001);

    if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        LOG("puerto %d: cerrado (errno=%d)\n", puerto, errno);
        close(s);
        return -1;
    }
    LOG("puerto %d: ABIERTO\n", puerto);
    return s;
}

/* Conecta, manda "envio" (si hay) y anota lo que conteste */
static void sondear(const char *nombre, int puerto, const char *envio, int espera) {
    LOG("--- %s ---\n", nombre);
    int s = conectar(puerto, espera);
    if (s < 0) return;

    if (envio) {
        ssize_t w = send(s, envio, strlen(envio), 0);
        LOG("  enviado: %d bytes (errno=%d)\n", (int)w, w < 0 ? errno : 0);
    }

    char resp[500];
    ssize_t r = recv(s, resp, sizeof(resp) - 1, 0);
    if (r <= 0) {
        LOG("  recibido: %d bytes (errno=%d)\n", (int)r, errno);
    } else {
        for (int k = 0; k < r; k++) {
            if (resp[k] < 32 || resp[k] > 126) resp[k] = '.';
        }
        resp[r] = 0;
        LOG("  recibido: %d bytes: %s\n", (int)r, resp);
    }
    close(s);
}

int main(void) {
    f = fopen("/data/ps4-usb-red.txt", "w");
    if (!f) return 1;
    LOG("inicio\n");

    /* Control: el FTP de GoldHEN saluda solo al conectar */
    sondear("FTP 2121 (control)", 2121, NULL, 5);

    /* 12800: primero una peticion simple */
    sondear("12800 GET /", 12800,
            "GET / HTTP/1.1\r\nHost: 127.0.0.1:12800\r\nConnection: close\r\n\r\n", 10);

    /* 12800: la pregunta is_exists escrita como lo hace curl */
    const char *cuerpo = "{\"title_id\":\"CUSA09311\"}";
    char req[400];
    snprintf(req, sizeof(req),
             "POST /api/is_exists HTTP/1.1\r\nHost: 127.0.0.1:12800\r\n"
             "User-Agent: curl/7.68.0\r\nAccept: */*\r\n"
             "Content-Length: %d\r\n"
             "Content-Type: application/x-www-form-urlencoded\r\n\r\n%s",
             (int)strlen(cuerpo), cuerpo);
    sondear("12800 POST is_exists", 12800, req, 10);

    LOG("fin\n");
    fclose(f);
    return 0;
}
