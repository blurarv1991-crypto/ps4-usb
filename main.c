#include <stdio.h>
#include <stdint.h>
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/Usbd.h>

int main(void) {
    FILE *f = fopen("/data/ps4-usb.txt", "w");
    if (!f) return 1;

    sceSysmoduleLoadModule(ORBIS_SYSMODULE_USBD);
    int r = sceUsbdInit();
    fprintf(f, "init: 0x%x\n", r);

    libusb_device **lista = NULL;
    int n = sceUsbdGetDeviceList(&lista);
    fprintf(f, "dispositivos=%d\n", n);
    fflush(f);

    libusb_device *mtp = NULL;
    int mtp_if = -1;

    for (int j = 0; j < n; j++) {
        struct libusb_device_descriptor d;
        if (sceUsbdGetDeviceDescriptor(lista[j], &d) != 0) continue;
        fprintf(f, "dispositivo %d: VID=%04x PID=%04x\n", j, d.idVendor, d.idProduct);

        struct libusb_config_descriptor *c = NULL;
        r = sceUsbdGetActiveConfigDescriptor(lista[j], &c);
        if (r != 0 || !c) {
            fprintf(f, "  sin config: 0x%x\n", r);
            continue;
        }

        for (int i = 0; i < c->bNumInterfaces; i++) {
            const struct libusb_interface *itf = &c->interface[i];
            for (int a = 0; a < itf->num_altsetting; a++) {
                const struct libusb_interface_descriptor *id = &itf->altsetting[a];
                fprintf(f, "  interfaz %d clase=%02x sub=%02x proto=%02x endpoints=%d\n",
                        id->bInterfaceNumber, id->bInterfaceClass,
                        id->bInterfaceSubClass, id->bInterfaceProtocol,
                        id->bNumEndpoints);
                for (int e = 0; e < id->bNumEndpoints; e++) {
                    const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
                    fprintf(f, "    ep=%02x tipo=%02x max=%d\n",
                            ep->bEndpointAddress, ep->bmAttributes, ep->wMaxPacketSize);
                }
                if (d.idVendor == 0x2717 && !mtp &&
                    (id->bInterfaceClass == 0x06 || id->bInterfaceClass == 0xff)) {
                    mtp = lista[j];
                    mtp_if = id->bInterfaceNumber;
                }
            }
        }
        sceUsbdFreeConfigDescriptor(c);
        fflush(f);
    }

    if (mtp) {
        libusb_device_handle *h = NULL;
        r = sceUsbdOpen(mtp, &h);
        fprintf(f, "abrir celular: 0x%x\n", r);
        if (r == 0 && h) {
            r = sceUsbdClaimInterface(h, mtp_if);
            fprintf(f, "reclamar interfaz %d: 0x%x\n", mtp_if, r);
            if (r == 0) sceUsbdReleaseInterface(h, mtp_if);
            sceUsbdClose(h);
        }
    } else {
        fprintf(f, "no encontre interfaz MTP en el celular\n");
    }

    if (n >= 0) sceUsbdFreeDeviceList(lista);
    sceUsbdExit();
    fprintf(f, "fin\n");
    fclose(f);
    return 0;
}
