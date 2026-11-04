#include <stdio.h>
#include <stdint.h>
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/Usbd.h>

#define EP_IN  0x81
#define EP_OUT 0x01

static void wr16(uint8_t *b, uint16_t v) { b[0] = v & 0xff; b[1] = (v >> 8) & 0xff; }
static void wr32(uint8_t *b, uint32_t v) { for (int i = 0; i < 4; i++) b[i] = (v >> (8 * i)) & 0xff; }
static uint16_t rd16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static uint32_t rd32(const uint8_t *b) { return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24); }

static int enviar(FILE *f, libusb_device_handle *h, uint16_t code, uint32_t tid, int np, uint32_t p1) {
    uint8_t b[16];
    uint32_t len = 12 + 4 * np;
    wr32(b, len);
    wr16(b + 4, 1);
    wr16(b + 6, code);
    wr32(b + 8, tid);
    if (np) wr32(b + 12, p1);
    int32_t tr = 0;
    int r = sceUsbdBulkTransfer(h, EP_OUT, b, (int32_t)len, &tr, 5000);
    fprintf(f, "enviar 0x%04x: r=0x%x bytes=%d\n", code, r, tr);
    fflush(f);
    return r;
}

static int recibir(FILE *f, libusb_device_handle *h, uint8_t *buf, int32_t *got) {
    *got = 0;
    int r = sceUsbdBulkTransfer(h, EP_IN, buf, 512, got, 5000);
    if (r != 0 || *got < 12) {
        fprintf(f, "recibir: r=0x%x bytes=%d\n", r, *got);
        fflush(f);
        return -1;
    }
    fprintf(f, "recibido: bytes=%d tipo=%d codigo=0x%04x\n", *got, rd16(buf + 4), rd16(buf + 6));
    fflush(f);
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
                uint8_t buf[512];
                int32_t got = 0;

                /* OpenSession (0x1002), sesion 1. Respuesta buena: 0x2001 */
                enviar(f, h, 0x1002, 1, 1, 1);
                recibir(f, h, buf, &got);

                /* GetStorageIDs (0x1004) */
                enviar(f, h, 0x1004, 2, 0, 0);
                if (recibir(f, h, buf, &got) == 0) {
                    if (rd16(buf + 4) == 2 && got >= 16) {
                        uint32_t cuenta = rd32(buf + 12);
                        fprintf(f, "almacenamientos=%u\n", cuenta);
                        for (uint32_t i = 0; i < cuenta && (int32_t)(16 + 4 * i + 4) <= got; i++) {
                            fprintf(f, "  id=0x%08x\n", rd32(buf + 16 + 4 * i));
                        }
                        recibir(f, h, buf, &got);
                    }
                }
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
