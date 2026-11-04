#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/Usbd.h>

#define EP_IN  0x81
#define EP_OUT 0x01
#define CAP    16384
#define CH     262144
#define MAXH   256
#define CARPETA_PKG "/data/pkg"
#define RUTA_PKG    "/data/pkg/ps4-usb-prueba.pkg"

static uint8_t datos[CAP];
static uint8_t chunk[CH];
static uint32_t tid = 0;
static int ruido = 1;

static void wr16(uint8_t *b, uint16_t v) { b[0] = v & 0xff; b[1] = (v >> 8) & 0xff; }
static void wr32(uint8_t *b, uint32_t v) { for (int i = 0; i < 4; i++) b[i] = (v >> (8 * i)) & 0xff; }
static uint16_t rd16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static uint32_t rd32(const uint8_t *b) { return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24); }

static unsigned long long kbs(uint64_t bytes, uint64_t us) {
    if (us == 0) us = 1;
    return (unsigned long long)(bytes * 1000000ULL / 1024ULL / us);
}

static int enviar(FILE *f, libusb_device_handle *h, uint16_t code, int np, uint32_t p1, uint32_t p2, uint32_t p3) {
    uint8_t b[24];
    uint32_t len = 12 + 4 * np;
    tid++;
    wr32(b, len);
    wr16(b + 4, 1);
    wr16(b + 6, code);
    wr32(b + 8, tid);
    if (np > 0) wr32(b + 12, p1);
    if (np > 1) wr32(b + 16, p2);
    if (np > 2) wr32(b + 20, p3);
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
    if (enviar(f, h, 0x1008, 1, handle, 0, 0) != 0) return -1;
    if (recibir_datos(f, h, &total) != 0) return -1;
    const uint8_t *p = datos + 12;
    if (total < 12 + 53) return -1;
    *fmt = rd16(p + 4);
    *tam = rd32(p + 8);
    int ln = p[52];
    int k = 0;
    if (12 + 53 + 2 * ln <= total) {
        for (int i = 0; i < ln && k < 62; i++) {
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
    if (enviar(f, h, 0x1007, 3, 0x00010001, 0, parent) != 0) return -1;
    if (recibir_datos(f, h, &total) != 0) return -1;
    *cuenta = rd32(datos + 12);
    for (uint32_t i = 0; i < *cuenta && i < MAXH && (int32_t)(16 + 4 * i + 4) <= total; i++) {
        hs[(*nh)++] = rd32(datos + 16 + 4 * i);
    }
    respuesta(f, h);
    return 0;
}

/* Baja un archivo y lo guarda en la PS4, midiendo USB y escritura por separado */
static int guardar(FILE *f, libusb_device_handle *h, uint32_t handle, uint32_t tam, const char *ruta) {
    FILE *o = fopen(ruta, "wb");
    if (!o) {
        fprintf(f, "no pude crear %s\n", ruta);
        return -1;
    }
    if (enviar(f, h, 0x1009, 1, handle, 0, 0) != 0) {
        fclose(o);
        return -1;
    }
    uint64_t t0 = sceKernelGetProcessTime();
    uint64_t tw = 0;
    int32_t got = 0;
    int r = leer(h, chunk, CH, &got);
    if (r != 0 || got < 12 || rd16(chunk + 4) != 2) {
        fprintf(f, "guardar: r=0x%x bytes=%d\n", r, got);
        fclose(o);
        return -1;
    }
    uint32_t falta = rd32(chunk) - 12;
    uint32_t hecho = (uint32_t)(got - 12);
    uint64_t a = sceKernelGetProcessTime();
    size_t w = fwrite(chunk + 12, 1, (size_t)(got - 12), o);
    tw += sceKernelGetProcessTime() - a;
    if (w != (size_t)(got - 12)) fprintf(f, "error escribiendo\n");

    while (hecho < falta) {
        got = 0;
        r = leer(h, chunk, CH, &got);
        if (r != 0 || got <= 0) {
            fprintf(f, "guardar cortado: r=0x%x despues de %u bytes\n", r, hecho);
            break;
        }
        a = sceKernelGetProcessTime();
        w = fwrite(chunk, 1, (size_t)got, o);
        tw += sceKernelGetProcessTime() - a;
        if (w != (size_t)got) {
            fprintf(f, "error escribiendo despues de %u bytes\n", hecho);
            break;
        }
        hecho += (uint32_t)got;
    }
    a = sceKernelGetProcessTime();
    fclose(o);
    tw += sceKernelGetProcessTime() - a;

    uint64_t total = sceKernelGetProcessTime() - t0;
    uint64_t tusb = total > tw ? total - tw : 1;
    fprintf(f, "guardado %u de %u bytes en %s\n", hecho, tam, ruta);
    fprintf(f, "total: %llu ms = %llu KB/s\n", (unsigned long long)(total / 1000), kbs(hecho, total));
    fprintf(f, "tiempo USB: %llu ms = %llu KB/s\n", (unsigned long long)(tusb / 1000), kbs(hecho, tusb));
    fprintf(f, "tiempo escribiendo: %llu ms = %llu KB/s\n", (unsigned long long)(tw / 1000), kbs(hecho, tw));
    fflush(f);
    respuesta(f, h);
    return 0;
}

int main(void) {
    FILE *f = fopen("/data/ps4-usb.txt", "w");
    if (!f) return 1;

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
                char nombre[64];

                enviar(f, h, 0x1002, 1, 1, 0, 0);
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
                } else {
                    uint32_t cand = 0, ctam = 0;
                    if (listar(f, h, dl, hs, &nh, &cuenta) == 0) {
                        fprintf(f, "Download: %u objetos (reviso %u)\n", cuenta, nh);
                        for (uint32_t i = 0; i < nh && !cand; i++) {
                            if (info(f, h, hs[i], &fmt, &tam, nombre) != 0) continue;
                            int len = (int)strlen(nombre);
                            if (fmt != 0x3001 && tam > 1048576 && len > 4 &&
                                strcmp(nombre + len - 4, ".pkg") == 0) {
                                cand = hs[i];
                                ctam = tam;
                                fprintf(f, "elegido: %s (%u bytes)\n", nombre, tam);
                            }
                        }
                    }

                    if (cand) {
                        ruido = 1;
                        mkdir(CARPETA_PKG, 0777);
                        guardar(f, h, cand, ctam, RUTA_PKG);
                    } else {
                        fprintf(f, "no encontre un .pkg grande en Download\n");
                    }
                }

                ruido = 1;
                enviar(f, h, 0x1003, 0, 0, 0, 0);
                respuesta(f, h);
                sceUsbdReleaseInterface(h, 0);
            }
            sceUsbdClose(h);
        }
    } else {
        fprintf(f, "no encontre el celular\n");
    }

    if (n >= 0) sceUsbdFreeDeviceList(lista);
    sceUsbdExit();
    fprintf(f, "fin\n");
    fclose(f);
    return 0;
}
