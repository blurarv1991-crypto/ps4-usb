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

static int conectar(int puerto) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        LOG("puerto %d: no pude crear socket (errno=%d)\n", puerto, errno);
        return -1;
    }
    struct timeval tv;
    tv.tv_sec = 3;
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

int main(void) {
    f = fopen("/data/ps4-usb-red.txt", "w");
    if (!f) return 1;
    LOG("inicio\n");

    int puertos[] = {12800, 9090, 9021, 9020, 2121, 3232};
    int n = (int)(sizeof(puertos) / sizeof(puertos[0]));

    for (int i = 0; i < n; i++) {
        int s = conectar(puertos[i]);
        if (s < 0) continue;

        if (puertos[i] == 12800) {
            /* Pregunta inofensiva al Remote Package Installer: "existe esta app?" */
            const char *cuerpo = "{\"title_id\":\"CUSA09311\"}";
            char req[300];
            snprintf(req, sizeof(req),
                     "POST /api/is_exists HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                     "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
                     (int)strlen(cuerpo), cuerpo);
            ssize_t w = send(s, req, strlen(req), 0);
            LOG("  enviado: %d bytes\n", (int)w);

            char resp[400];
            ssize_t r = recv(s, resp, sizeof(resp) - 1, 0);
            LOG("  recibido: %d bytes\n", (int)r);
            if (r > 0) {
                for (int k = 0; k < r; k++) {
                    if (resp[k] < 32 || resp[k] > 126) resp[k] = '.';
                }
                resp[r] = 0;
                LOG("  respuesta: %s\n", resp);
            }
        }
        close(s);
    }

    LOG("fin\n");
    fclose(f);
    return 0;
}
