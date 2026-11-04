#include <stdio.h>
#include <stdint.h>
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/Usbd.h>

#define EP_IN  0x81
#define EP_OUT 0x01
#define CAP    16384
#define MAXH   64
#define MAXV   30

static uint8_t datos[CAP];
static uint32_t tid = 0;

static void wr16(uint8_t *b, uint16_t v) { b[0] = v & 0xff; b[1] = (v >> 8) & 0xff; }
static void wr32(uint8_t *b, uint32_t v) { for (int i = 0; i < 4; i++) b[i] = (v >> (8 * i)) & 0xff; }
static uint16_t rd16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static uint32_t rd32(const uint8_t *b) { return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24); }

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
    fprintf(f, "enviar 0x%04x: r=0x%x bytes=%d\n", code, r, tr);
    fflush(f);
    return r;
}

static int leer(libusb_device_handle *h, uint8_t *buf, int32_t *got) {
    int r = 0;
    *got = 0;
    for (int i = 0; i < 3; i++) {
        r = sceUsbdBulkTransfer(h, EP_IN, buf, 512, got, 5000);
        if (r != 0 || *got > 0) break;
    }
    return r;
}

static int respuesta(FILE *f, libusb_device_handle *h) {
    uint8_t b[512];
    int32_t got = 0;
    int r = leer(h, b, &got);
    if (r != 0 || got < 12) {
        fprintf(f, "respuesta: r=0x%x bytes=%d\n", r, got);
        fflush(f);
        return -1;
    }
    fprintf(f, "respuesta: tipo=%d codigo=0x%04x\n", rd16(b + 4), rd16(b + 6));
    fflush(f);
    return rd16(b + 6);
}

static int recibir_datos(FILE *f, libusb_device_handle *h, int32_t *total) {
    int32_t got = 0;
    int r = leer(h, datos, &got);
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
        r = leer(h, datos + got, &g);
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

static void mostrar(FILE *f, uint32_t handle, int32_t total) {
    const uint8_t *p = datos + 12;
    if (total < 12 + 53) {
        fprintf(f, "  h=%u info corta (%d)\n", handle, total);
        return;
    }
    uint16_t fmt = rd16(p + 4);
    uint32_t tam = rd32(p + 8);
    int ln = p[52];
    char nombre[64];
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
    fprintf(f, "  h=%u %s tam=%u nombre=%s\n", handle,
            fmt == 0x3001 ? "CARPETA" : "archivo", tam, nombre);
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
                int32_t total = 0;

                enviar(f, h, 0x1002, 1, 1, 0, 0);
                respuesta(f, h);

                /* GetObjectHandles: almacenamiento, todos los formatos, raiz */
                enviar(f, h, 0x1007, 3, 0x00010001, 0, 0xFFFFFFFF);
                if (recibir_datos(f, h, &total) == 0) {
                    uint32_t cuenta = rd32(datos + 12);
                    uint32_t hs[MAXH];
                    uint32_t nh = 0;
                    for (uint32_t i = 0; i < cuenta && i < MAXH &&
                         (int32_t)(16 + 4 * i + 4) <= total; i++) {
                        hs[nh++] = rd32(datos + 16 + 4 * i);
                    }
                    fprintf(f, "objetos en la raiz=%u (muestro %u)\n", cuenta, nh);
                    respuesta(f, h);

                    for (uint32_t i = 0; i < nh && i < MAXV; i++) {
                        enviar(f, h, 0x1008, 1, hs[i], 0, 0);
                        if (recibir_datos(f, h, &total) == 0) {
                            mostrar(f, hs[i], total);
                            respuesta(f, h);
                        }
                    }
                }

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
