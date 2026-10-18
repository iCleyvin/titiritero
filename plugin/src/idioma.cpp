#include "idioma.h"

#include <coreinit/userconfig.h>

#include <cstring>

namespace {
    bool espanol = true;
}

void idioma_iniciar() {
    UCHandle h = UCOpen();
    if (h < 0) return;
    uint32_t idioma = 5;
    UCSysConfig cfg = {};
    strcpy(cfg.name, "cafe.language");
    cfg.dataType = UC_DATATYPE_UNSIGNED_INT;
    cfg.dataSize = sizeof idioma;
    cfg.data = &idioma;
    if (UCReadSysConfig(h, 1, &cfg) == UC_ERROR_OK) espanol = idioma == 5;   /* 5 = español */
    UCClose(h);
}

bool idioma_es() { return espanol; }
