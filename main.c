#include <stdio.h>
#include <stdint.h>
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/Usbd.h>

int main(void) {
    FILE *f = fopen("/data/ps4-usb.txt", "w");
    if (!f) return 1;

    fprintf(f, "inicio\n");
    fflush(f);

    int r = sceSysmoduleLoadModule(ORBIS_SYSMODULE_USBD);
    fprintf(f, "cargar usbd: 0x%x\n", r);

    r = sceUsbdInit();
    fprintf(f, "usbd init: 0x%x\n", r);
    fflush(f);

    for (int i = 0; i < 15; i++) {
        libusb_device **lista = NULL;
        int n = sceUsbdGetDeviceList(&lista);
        fprintf(f, "ciclo %d: dispositivos=%d\n", i, n);
        for (int j = 0; j < n; j++) {
            struct libusb_device_descriptor d;
            if (sceUsbdGetDeviceDescriptor(lista[j], &d) == 0) {
                fprintf(f, "  VID=%04x PID=%04x clase=%02x\n",
                        d.idVendor, d.idProduct, d.bDeviceClass);
            }
        }
        if (n >= 0) sceUsbdFreeDeviceList(lista);
        fflush(f);
        sceKernelUsleep(2000000);
    }

    sceUsbdExit();
    fprintf(f, "fin\n");
    fclose(f);
    return 0;
}
