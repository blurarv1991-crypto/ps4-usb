#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/Usbd.h>

#define EP_IN  0x81
#define EP_OUT 0x01
#define CAP    16384
#define CH     262144
#define MAXH   256
#define PARTE  67108864u
#define NOMBRE 256
#define CARPETA_PKG "/data/pkg"
#define RUTA_PKG    "/data/pkg/ps4-usb-prueba.pkg"
#define RUTA_PAYLOAD "/data/ps4-installer.elf"
#define PUERTO_PAYLOAD 9090

static uint8_t datos[CAP];
static uint8_t chunk[CH];
static uint32_t tid = 0;
static int ruido = 1;

static void wr16(uint8_t *b, uint16_t v) { b[0] = v & 0xff; b[1] = (v >> 8) & 0xff; }
static void wr32(uint8_t *b, uint32_t v) { for (int i = 0; i < 4; i++) b[i] = (v >> (8 * i)) & 0xff; }
static uint16_t rd16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static uint32_t rd32(const uint8_t *b) { return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24); }

typedef struct {
    char pad[45];
    char message[3075];
} notify_request_t;

static unsigned long long g_kbs = 0;

/* Muestra un mensaje en pantalla (notificacion de la PS4) */
static void avisa(const char *fmt, ...) {
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(req.message, 200, fmt, ap);
    va_end(ap);
    sceKernelSendNotificationRequest(0, (OrbisNotificationRequest *)&req, sizeof(req), 0);
}

/* Manda el payload instalador (/data/ps4-installer.elf) al BinLoader de GoldHEN */
static int enviar_payload(FILE *f) {
    FILE *p = fopen(RUTA_PAYLOAD, "rb");
    if (!p) {
        fprintf(f, "falta %s\n", RUTA_PAYLOAD);
        fflush(f);
        avisa("PS4 USB: falta %s (subelo por FTP)", RUTA_PAYLOAD);
        return -1;
    }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        fprintf(f, "payload: no pude crear socket (errno=%d)\n", errno);
        fflush(f);
        fclose(p);
        avisa("PS4 USB: no pude abrir la red para el payload");
        return -1;
    }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(PUERTO_PAYLOAD);
    a.sin_addr.s_addr = htonl(0x7F000001);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        fprintf(f, "payload: no conecta al puerto %d (errno=%d)\n", PUERTO_PAYLOAD, errno);
        fflush(f);
        close(s);
        fclose(p);
        avisa("PS4 USB: no conecta a GoldHEN (activa BinLoader, puerto 9090)");
        return -1;
    }
    static uint8_t buf[65536];
    long total = 0;
    int ok = 1;
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof(buf), p)) > 0) {
        size_t off = 0;
        while (off < n) {
            ssize_t w = send(s, buf + off, n - off, 0);
            if (w <= 0) {
                ok = 0;
                break;
            }
            off += (size_t)w;
        }
        total += (long)n;
    }
    sceKernelUsleep(500000);
    close(s);
    fclose(p);
    fprintf(f, "payload enviado: %ld bytes ok=%d\n", total, ok);
    fflush(f);
    if (!ok) avisa("PS4 USB: fallo al enviar el payload");
    return ok ? 0 : -1;
}

static unsigned long long kbs(uint64_t bytes, uint64_t us) {
    if (us == 0) us = 1;
    return (unsigned long long)(bytes * 1000000ULL / 1024ULL / us);
}

static int enviar(FILE *f, libusb_device_handle *h, uint16_t code, int np, uint32_t p1, uint32_t p2, uint32_t p3, uint32_t p4) {
    uint8_t b[28];
    uint32_t len = 12 + 4 * np;
    tid++;
    wr32(b, len);
    wr16(b + 4, 1);
    wr16(b + 6, code);
    wr32(b + 8, tid);
    if (np > 0) wr32(b + 12, p1);
    if (np > 1) wr32(b + 16, p2);
    if (np > 2) wr32(b + 20, p3);
    if (np > 3) wr32(b + 24, p4);
    int32_t tr = 0;
    int r = sceUsbdBulkTransfer(h, EP_OUT, b, (int32_t)len, &tr, 5000);
    if (ruido || r != 0) fprintf(f, "enviar 0x%04x: r=0x%x bytes=%d\n", code, r, tr);
    fflush(f);
    return r;
}

static int leer(libusb_device_handle *h, uint8_t *buf, int32_t max, int32_t *got) {
    int r = 0;
    *got = 0;
    for (int i = 0; i < 3; i++) {
        r = sceUsbdBulkTransfer(h, EP_IN, buf, max, got, 10000);
        if (r != 0 || *got > 0) break;
    }
    return r;
}

static int respuesta(FILE *f, libusb_device_handle *h) {
    uint8_t b[512];
    int32_t got = 0;
    int r = leer(h, b, 512, &got);
    if (r != 0 || got < 12) {
        fprintf(f, "respuesta: r=0x%x bytes=%d\n", r, got);
        fflush(f);
        return -1;
    }
    if (ruido || rd16(b + 6) != 0x2001)
        fprintf(f, "respuesta: tipo=%d codigo=0x%04x\n", rd16(b + 4), rd16(b + 6));
    fflush(f);
    return rd16(b + 6);
}

static int recibir_datos(FILE *f, libusb_device_handle *h, int32_t *total) {
    int32_t got = 0;
    int r = leer(h, datos, 512, &got);
    if (r != 0 || got < 12) {
        fprintf(f, "datos: r=0x%x bytes=%d\n", r, got);
        fflush(f);
        return -1;
    }
    if (rd16(datos + 4) == 3) {
        fprintf(f, "sin datos, respuesta 0x%04x\n", rd16(datos + 6));
        fflush(f);
        return -2;
    }
    uint32_t largo = rd32(datos);
    if (largo > CAP - 512) {
        fprintf(f, "datos demasiado grandes: %u\n", largo);
        fflush(f);
        return -1;
    }
    while (got < (int32_t)largo) {
        int32_t g = 0;
        r = leer(h, datos + got, 512, &g);
        if (r != 0 || g <= 0) {
            fprintf(f, "datos cortados: r=0x%x\n", r);
            fflush(f);
            return -1;
        }
        got += g;
    }
    *total = got;
    return 0;
}

static int info(FILE *f, libusb_device_handle *h, uint32_t handle, uint16_t *fmt, uint32_t *tam, char *nombre) {
    int32_t total = 0;
    nombre[0] = 0;
    if (enviar(f, h, 0x1008, 1, handle, 0, 0, 0) != 0) return -1;
    if (recibir_datos(f, h, &total) != 0) return -1;
    const uint8_t *p = datos + 12;
    if (total < 12 + 53) return -1;
    *fmt = rd16(p + 4);
    *tam = rd32(p + 8);
    int ln = p[52];
    int k = 0;
    if (12 + 53 + 2 * ln <= total) {
        for (int i = 0; i < ln && k < NOMBRE - 1; i++) {
            uint8_t lo = p[53 + 2 * i];
            uint8_t hi = p[53 + 2 * i + 1];
            if (lo == 0 && hi == 0) break;
            nombre[k++] = (hi == 0 && lo >= 32 && lo < 127) ? (char)lo : '?';
        }
    }
    nombre[k] = 0;
    respuesta(f, h);
    return 0;
}

static int listar(FILE *f, libusb_device_handle *h, uint32_t parent, uint32_t *hs, uint32_t *nh, uint32_t *cuenta) {
    int32_t total = 0;
    *nh = 0;
    *cuenta = 0;
    if (enviar(f, h, 0x1007, 3, 0x00010001, 0, parent, 0) != 0) return -1;
    if (recibir_datos(f, h, &total) != 0) return -1;
    *cuenta = rd32(datos + 12);
    for (uint32_t i = 0; i < *cuenta && i < MAXH && (int32_t)(16 + 4 * i + 4) <= total; i++) {
        hs[(*nh)++] = rd32(datos + 16 + 4 * i);
    }
    respuesta(f, h);
    return 0;
}

static int es_pkg(const char *n) {
    int len = (int)strlen(n);
    if (len <= 4) return 0;
    const char *e = n + len - 4;
    return e[0] == '.' && (e[1] == 'p' || e[1] == 'P') && (e[2] == 'k' || e[2] == 'K') && (e[3] == 'g' || e[3] == 'G');
}

/* Tamano real (64 bits) de un objeto: hace falta para archivos de mas de 4 GB */
static int tam64(FILE *f, libusb_device_handle *h, uint32_t handle, uint64_t *out) {
    int32_t total = 0;
    if (enviar(f, h, 0x9803, 2, handle, 0xDC04, 0, 0) != 0) return -1;
    if (recibir_datos(f, h, &total) != 0) return -1;
    int ok = total >= 12 + 8;
    if (ok) *out = (uint64_t)rd32(datos + 12) | ((uint64_t)rd32(datos + 16) << 32);
    respuesta(f, h);
    return ok ? 0 : -1;
}

/* Baja un archivo por partes de 64 MB (GetPartialObject64) y lo guarda en la PS4 */
static int guardar(FILE *f, libusb_device_handle *h, uint32_t handle, uint64_t tam, const char *ruta) {
    FILE *o = fopen(ruta, "wb");
    if (!o) {
        fprintf(f, "no pude crear %s\n", ruta);
        return -1;
    }
    uint64_t t0 = sceKernelGetProcessTime();
    uint64_t tw = 0;
    uint64_t hecho = 0;
    uint64_t aviso = 1073741824ULL;
    int ok = 1;

    while (hecho < tam) {
        uint64_t resto = tam - hecho;
        uint32_t pedir = resto > PARTE ? PARTE : (uint32_t)resto;
        if (enviar(f, h, 0x95C1, 4, handle, (uint32_t)(hecho & 0xFFFFFFFFULL), (uint32_t)(hecho >> 32), pedir) != 0) {
            ok = 0;
            break;
        }
        int32_t got = 0;
        int r = leer(h, chunk, CH, &got);
        if (r != 0 || got < 12 || rd16(chunk + 4) != 2) {
            fprintf(f, "guardar: r=0x%x bytes=%d codigo=0x%04x despues de %llu bytes\n", r, got,
                    got >= 8 ? rd16(chunk + 6) : 0, (unsigned long long)hecho);
            ok = 0;
            break;
        }
        uint32_t falta = rd32(chunk) - 12;
        uint32_t parte = (uint32_t)(got - 12);
        uint64_t a = sceKernelGetProcessTime();
        size_t w = fwrite(chunk + 12, 1, (size_t)(got - 12), o);
        tw += sceKernelGetProcessTime() - a;
        if (w != (size_t)(got - 12)) {
            fprintf(f, "error escribiendo despues de %llu bytes\n", (unsigned long long)hecho);
            ok = 0;
            break;
        }
        while (parte < falta) {
            got = 0;
            r = leer(h, chunk, CH, &got);
            if (r != 0 || got <= 0) {
                fprintf(f, "guardar cortado: r=0x%x despues de %llu bytes\n", r, (unsigned long long)(hecho + parte));
                ok = 0;
                break;
            }
            a = sceKernelGetProcessTime();
            w = fwrite(chunk, 1, (size_t)got, o);
            tw += sceKernelGetProcessTime() - a;
            if (w != (size_t)got) {
                fprintf(f, "error escribiendo despues de %llu bytes\n", (unsigned long long)(hecho + parte));
                ok = 0;
                break;
            }
            parte += (uint32_t)got;
        }
        hecho += parte;
        if (!ok) break;
        respuesta(f, h);
        if (parte == 0) {
            ok = 0;
            break;
        }
        if (hecho >= aviso) {
            avisa("PS4 USB: guardando... %llu de %llu MB", (unsigned long long)(hecho / 1048576ULL),
                  (unsigned long long)(tam / 1048576ULL));
            aviso += 1073741824ULL;
        }
    }

    uint64_t a2 = sceKernelGetProcessTime();
    fclose(o);
    tw += sceKernelGetProcessTime() - a2;

    uint64_t total = sceKernelGetProcessTime() - t0;
    uint64_t tusb = total > tw ? total - tw : 1;
    fprintf(f, "guardado %llu de %llu bytes en %s\n", (unsigned long long)hecho, (unsigned long long)tam, ruta);
    fprintf(f, "total: %llu ms = %llu KB/s\n", (unsigned long long)(total / 1000), kbs(hecho, total));
    fprintf(f, "tiempo USB: %llu ms = %llu KB/s\n", (unsigned long long)(tusb / 1000), kbs(hecho, tusb));
    fprintf(f, "tiempo escribiendo: %llu ms = %llu KB/s\n", (unsigned long long)(tw / 1000), kbs(hecho, tw));
    fflush(f);
    g_kbs = kbs(hecho, total);
    return (ok && hecho == tam) ? 0 : -1;
}

/* Prueba: que carpetas ve la app? (para encontrar el disco externo) */
static void ver_carpeta(FILE *f, const char *ruta) {
    char lista[180];
    size_t n = 0;
    lista[0] = 0;
    DIR *d = opendir(ruta);
    if (!d) {
        fprintf(f, "carpeta %s: no abre (errno=%d)\n", ruta, errno);
        fflush(f);
        avisa("PS4 USB: no puedo abrir %s (errno=%d)", ruta, errno);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        fprintf(f, "  %s/%s\n", ruta, e->d_name);
        size_t l = strlen(e->d_name);
        if (n + l + 2 < sizeof(lista)) {
            memcpy(lista + n, e->d_name, l);
            n += l;
            lista[n++] = ' ';
            lista[n] = 0;
        }
    }
    closedir(d);
    fflush(f);
    avisa("PS4 USB: %s contiene: %s", ruta, lista);
}

static void probar_externo(FILE *f) {
    ver_carpeta(f, "/mnt");
    ver_carpeta(f, "/");
}

int main(void) {
    FILE *f = fopen("/data/ps4-usb.txt", "w");
    if (!f) return 1;
    int guardado_ok = 0;
    avisa("PS4 USB: buscando el celular...");
    probar_externo(f);

    sceSysmoduleLoadModule(ORBIS_SYSMODULE_USBD);
    int r = sceUsbdInit();
    fprintf(f, "init: 0x%x\n", r);

    libusb_device **lista = NULL;
    int n = sceUsbdGetDeviceList(&lista);
    fprintf(f, "dispositivos=%d\n", n);

    libusb_device *cel = NULL;
    for (int j = 0; j < n; j++) {
        struct libusb_device_descriptor d;
        if (sceUsbdGetDeviceDescriptor(lista[j], &d) != 0) continue;
        if (d.idVendor == 0x2717) { cel = lista[j]; break; }
    }

    if (cel) {
        libusb_device_handle *h = NULL;
        r = sceUsbdOpen(cel, &h);
        fprintf(f, "abrir celular: 0x%x\n", r);
        if (r == 0 && h) {
            r = sceUsbdClaimInterface(h, 0);
            fprintf(f, "reclamar interfaz 0: 0x%x\n", r);
            if (r == 0) {
                uint32_t hs[MAXH];
                uint32_t nh = 0, cuenta = 0, dl = 0;
                uint16_t fmt = 0;
                uint32_t tam = 0;
                char nombre[NOMBRE];

                enviar(f, h, 0x1002, 1, 1, 0, 0, 0);
                respuesta(f, h);
                ruido = 0;

                if (listar(f, h, 0xFFFFFFFF, hs, &nh, &cuenta) == 0) {
                    for (uint32_t i = 0; i < nh; i++) {
                        if (info(f, h, hs[i], &fmt, &tam, nombre) != 0) continue;
                        if (fmt == 0x3001 && strcmp(nombre, "Download") == 0) {
                            dl = hs[i];
                            break;
                        }
                    }
                }

                if (!dl) {
                    fprintf(f, "no encontre la carpeta Download\n");
                    avisa("PS4 USB: no encontre la carpeta Download en el celular");
                } else {
                    uint32_t cand = 0;
                    uint64_t ctam = 0;
                    char cnombre[NOMBRE];
                    cnombre[0] = 0;
                    if (listar(f, h, dl, hs, &nh, &cuenta) == 0) {
                        fprintf(f, "Download: %u objetos (reviso %u)\n", cuenta, nh);
                        for (uint32_t i = 0; i < nh; i++) {
                            if (info(f, h, hs[i], &fmt, &tam, nombre) != 0) continue;
                            if (fmt == 0x3001 || !es_pkg(nombre)) continue;
                            uint64_t t = tam;
                            if (tam == 0xFFFFFFFFu) {
                                t = 0;
                                uint64_t t64 = 0;
                                if (tam64(f, h, hs[i], &t64) == 0) t = t64;
                                else fprintf(f, "no pude leer el tamano de %s\n", nombre);
                            }
                            fprintf(f, "pkg: %s (%llu bytes)\n", nombre, (unsigned long long)t);
                            if (t > 1048576ULL && t > ctam) {
                                cand = hs[i];
                                ctam = t;
                                strncpy(cnombre, nombre, NOMBRE - 1);
                                cnombre[NOMBRE - 1] = 0;
                            }
                        }
                        if (cand) fprintf(f, "elegido: %s (%llu bytes)\n", cnombre, (unsigned long long)ctam);
                    }

                    if (cand) {
                        ruido = 1;
                        mkdir(CARPETA_PKG, 0777);
                        avisa("PS4 USB: guardando %s (%llu MB)...", cnombre, (unsigned long long)(ctam / 1048576ULL));
                        if (guardar(f, h, cand, ctam, RUTA_PKG) == 0) {
                            guardado_ok = 1;
                            avisa("PS4 USB: guardado (%llu MB/s)", g_kbs / 1024);
                        } else {
                            avisa("PS4 USB: fallo al guardar el .pkg (mira /data/ps4-usb.txt)");
                        }
                    } else {
                        fprintf(f, "no encontre un .pkg grande en Download\n");
                        avisa("PS4 USB: no hay un .pkg grande en la carpeta Download del celular");
                    }
                }

                ruido = 1;
                enviar(f, h, 0x1003, 0, 0, 0, 0, 0);
                respuesta(f, h);
                sceUsbdReleaseInterface(h, 0);
            }
            sceUsbdClose(h);
        }
    } else {
        fprintf(f, "no encontre el celular\n");
        avisa("PS4 USB: no encontre el celular (cable y modo Transferencia de archivos)");
    }

    if (n >= 0) sceUsbdFreeDeviceList(lista);
    sceUsbdExit();

    if (guardado_ok) {
        avisa("PS4 USB: enviando el instalador...");
        enviar_payload(f);
        sceKernelSleep(20);
    } else {
        sceKernelSleep(5);
    }

    fprintf(f, "fin\n");
    fclose(f);
    return 0;
}
