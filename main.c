#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/AppInstUtil.h>

#define RUTA_PKG "/data/pkg/ps4-usb-prueba.pkg"

static FILE *f;
#define LOG(...) do { fprintf(f, __VA_ARGS__); fflush(f); } while (0)

int main(void) {
    f = fopen("/data/ps4-usb-instalar.txt", "w");
    if (!f) return 1;

    LOG("inicio\n");

    int r = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_APP_INST_UTIL);
    LOG("1 cargar AppInstUtil: 0x%x\n", r);

    r = sceAppInstUtilInitialize();
    LOG("2 AppInstUtil init: 0x%x\n", r);

    char titleId[64];
    memset(titleId, 0, sizeof(titleId));
    int32_t esApp = 0;
    r = sceAppInstUtilGetTitleIdFromPkg(RUTA_PKG, titleId, &esApp);
    LOG("3 titulo del pkg: 0x%x titleId=%s esApp=%d\n", r, titleId, esApp);

    int32_t existe = -1;
    r = sceAppInstUtilAppExists(titleId, &existe);
    LOG("4 ya instalado antes: 0x%x existe=%d\n", r, existe);

    LOG("5 pidiendo instalacion...\n");
    r = sceAppInstUtilAppInstallPkg(RUTA_PKG, NULL);
    LOG("5 pedir instalacion: 0x%x\n", r);

    sceKernelUsleep(8000000);
    existe = -1;
    r = sceAppInstUtilAppExists(titleId, &existe);
    LOG("6 instalado despues: 0x%x existe=%d\n", r, existe);

    LOG("fin\n");
    fclose(f);
    return 0;
}
