#include "titulos.h"
#include "comun.h"

#include <coreinit/mcp.h>
#include <nn/acp/client.h>
#include <nn/acp/title.h>

#include <dirent.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <strings.h>

namespace {
    void limpiar_nombre(char *s) {
        for (; *s; ++s)
            if (*s == '\n' || *s == '\r') *s = ' ';
    }

    bool es_ejecutable(const char *nombre) {
        const char *punto = strrchr(nombre, '.');
        return punto && (!strcasecmp(punto, ".wuhb") || !strcasecmp(punto, ".rpx"));
    }

    void app_json(Texto *t, const char *ruta, const char *nombre, bool *primero) {
        tx_add(t, "%s{\"nombre\":", *primero ? "" : ",");
        tx_json(t, nombre);
        tx_add(t, ",\"ruta\":");
        tx_json(t, ruta);
        tx_add(t, "}");
        *primero = false;
    }

    /* Primer .wuhb o .rpx de una carpeta de app. */
    bool ejecutable_en(const char *carpeta, char *archivo, uint32_t cap) {
        DIR *d = opendir(carpeta);
        if (!d) return false;
        bool hallado = false;
        for (dirent *e = readdir(d); e && !hallado; e = readdir(d)) {
            if (!es_ejecutable(e->d_name)) continue;
            snprintf(archivo, cap, "%s", e->d_name);
            hallado = true;
        }
        closedir(d);
        return hallado;
    }
} // namespace

bool nombre_titulo(uint64_t id, char *nombre, uint32_t cap) {
    auto *meta = (ACPMetaXml *) memalign(0x40, sizeof(ACPMetaXml));
    if (!meta) return false;
    ACPInitialize();
    bool ok = ACPGetTitleMetaXml(id, meta) == ACP_RESULT_SUCCESS;
    if (ok) {
        snprintf(nombre, cap, "%s", meta->longname_es[0] ? meta->longname_es : meta->longname_en);
        limpiar_nombre(nombre);
    }
    ACPFinalize();
    free(meta);
    return ok && nombre[0];
}

/* Pedir los nombres cuesta ~2 s con 36 juegos: se guarda la lista hasta que se pida «fresca». */
static Texto cache;

void titulos_json(Texto *t, bool fresca) {
    if (cache.d && !fresca) return tx_add(t, "%s", cache.d);
    tx_libre(&cache);
    titulos_calcular(&cache);
    if (cache.d) tx_add(t, "%s", cache.d);
}

void titulos_calcular(Texto *t) {
    int mcp = MCP_Open();
    if (mcp < 0) return tx_add(t, "[]");
    int total = MCP_TitleCount(mcp);
    auto *lista = (MCPTitleListType *) calloc(total > 0 ? total : 1, sizeof(MCPTitleListType));
    uint32_t n = 0;
    if (lista) MCP_TitleListByAppType(mcp, MCP_APP_TYPE_GAME, &n, lista, total * sizeof(MCPTitleListType));
    MCP_Close(mcp);

    tx_add(t, "[");
    for (uint32_t i = 0; i < n; ++i) {
        char nombre[256] = "";
        if (!nombre_titulo(lista[i].titleId, nombre, sizeof nombre)) snprintf(nombre, sizeof nombre, "?");
        tx_add(t, "%s{\"id\":\"%016llx\",\"nombre\":", i ? "," : "", lista[i].titleId);
        tx_json(t, nombre);
        tx_add(t, ",\"dispositivo\":");
        tx_json(t, lista[i].indexedDevice);
        tx_add(t, "}");
    }
    tx_add(t, "]");
    free(lista);
}

void apps_json(Texto *t) {
    const char *base = "fs:/vol/external01/wiiu/apps";
    DIR *d = opendir(base);
    tx_add(t, "[");
    bool primero = true;
    for (dirent *e = d ? readdir(d) : nullptr; e; e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        char ruta[800], archivo[256];
        snprintf(ruta, sizeof ruta, "%s/%s", base, e->d_name);
        struct stat st = {};
        if (stat(ruta, &st) != 0) continue;
        if (!S_ISDIR(st.st_mode)) {
            if (!es_ejecutable(e->d_name)) continue;
            snprintf(ruta, sizeof ruta, "wiiu/apps/%s", e->d_name);
        } else if (ejecutable_en(ruta, archivo, sizeof archivo)) {
            snprintf(ruta, sizeof ruta, "wiiu/apps/%s/%s", e->d_name, archivo);
        } else {
            continue;
        }
        app_json(t, ruta, e->d_name, &primero);
    }
    if (d) closedir(d);
    tx_add(t, "]");
}
