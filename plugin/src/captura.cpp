/*
 * Captura de pantalla. El método (copiar el búfer de color a una superficie lineal
 * con GX2CopySurface justo antes de mostrarlo, resolviendo el antialiasing si lo hay)
 * es el de ScreenshotWUPS de Maschell (GPL-3.0, github.com/wiiu-env/ScreenshotWUPS).
 */
#include "captura.h"
#include "comun.h"
#include "superposicion.h"

#include <coreinit/cache.h>
#include <coreinit/event.h>
#include <coreinit/mutex.h>
#include <coreinit/time.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/surface.h>
#include <memory/mappedmemory.h>
#include <turbojpeg.h>
#include <wups.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
    enum Estado { LIBRE, PEDIDA, LISTA };

    struct Copia {
        void *imagen;
        uint32_t ancho, alto, pitch;
        bool a_srgb;
    };

    Estado estado = LIBRE;
    int pantalla_pedida;
    Copia copia;
    OSMutex cerrojo;
    OSEvent hecho;
    GX2SurfaceFormat formato_scan[2] = {GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8};
    uint8_t lineal_a_srgb[256];

    void preparar_destino(GX2Surface *d, uint32_t ancho, uint32_t alto) {
        memset(d, 0, sizeof *d);
        d->use = (GX2SurfaceUse) (GX2_SURFACE_USE_COLOR_BUFFER | GX2_SURFACE_USE_TEXTURE);
        d->dim = GX2_SURFACE_DIM_TEXTURE_2D;
        d->width = ancho;
        d->height = alto;
        d->depth = 1;
        d->mipLevels = 1;
        d->format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
        d->aa = GX2_AA_MODE1X;
        d->tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
        GX2CalcSurfaceSizeAndAlignment(d);
    }

    /* Resuelve el antialiasing en una superficie temporal y copia desde ella. */
    bool copiar_con_aa(const GX2ColorBuffer *origen, GX2Surface *destino) {
        GX2Surface temp = origen->surface;
        temp.aa = GX2_AA_MODE1X;
        GX2CalcSurfaceSizeAndAlignment(&temp);
        temp.image = MEMAllocFromMappedMemoryForGX2Ex(temp.imageSize, temp.alignment);
        if (!temp.image) return false;
        GX2ResolveAAColorBuffer(origen, &temp, 0, 0);
        GX2CopySurface(&temp, 0, 0, destino, 0, 0);
        GX2DrawDone();
        MEMFreeToMappedMemory(temp.image);
        return true;
    }

    bool copiar(const GX2ColorBuffer *origen, int pantalla) {
        GX2Surface destino;
        preparar_destino(&destino, origen->surface.width, origen->surface.height);
        destino.image = MEMAllocFromMappedMemoryForGX2Ex(destino.imageSize, destino.alignment);
        if (!destino.image) return false;
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU, destino.image, destino.imageSize);
        if (origen->surface.aa == GX2_AA_MODE1X) {
            GX2CopySurface(&origen->surface, origen->viewMip, origen->viewFirstSlice, &destino, 0, 0);
        } else if (!copiar_con_aa(origen, &destino)) {
            MEMFreeToMappedMemory(destino.image);
            return false;
        }
        GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, destino.image, destino.imageSize);
        GX2DrawDone();
        DCInvalidateRange(destino.image, destino.imageSize);
        copia = {destino.image, destino.width, destino.height, destino.pitch,
                 (formato_scan[pantalla] & 0x400) != 0};
        return true;
    }

    void convertir_a_srgb(const Copia &c) {
        auto *px = (uint8_t *) c.imagen;
        for (uint32_t y = 0; y < c.alto; ++y) {
            uint8_t *fila = px + y * c.pitch * 4;
            for (uint32_t x = 0; x < c.ancho * 4; ++x) fila[x] = lineal_a_srgb[fila[x]];
        }
    }

    /* Reduce a la mitad promediando bloques de 2x2, sobre el mismo búfer. */
    void reducir_a_mitad(Copia *c) {
        auto *px = (uint8_t *) c->imagen;
        uint32_t w = c->ancho / 2, h = c->alto / 2, fila = c->pitch * 4;
        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t *a = px + (2 * y) * fila, *b = a + fila;
            uint8_t *d = px + y * fila;
            for (uint32_t x = 0; x < w; ++x)
                for (int k = 0; k < 4; ++k)
                    d[x * 4 + k] = (uint8_t) ((a[x * 8 + k] + a[x * 8 + 4 + k] + b[x * 8 + k] + b[x * 8 + 4 + k] + 2) / 4);
        }
        c->ancho = w;
        c->alto = h;
    }

    uint8_t *codificar(const Copia &c, int calidad, uint32_t *tamano) {
        tjhandle tj = tjInitCompress();
        if (!tj) return nullptr;
        unsigned char *jpeg = nullptr;
        unsigned long largo = 0;
        int r = tjCompress2(tj, (const unsigned char *) c.imagen, (int) c.ancho, (int) (c.pitch * 4), (int) c.alto,
                            TJPF_RGBX, &jpeg, &largo, TJSAMP_420, calidad, TJFLAG_FASTDCT);
        tjDestroy(tj);
        if (r != 0) {
            tjFree(jpeg);
            return nullptr;
        }
        *tamano = (uint32_t) largo;
        return jpeg;
    }

    /* Devuelve la copia a LIBRE; si llegó tarde (tras un tiempo agotado), la libera. */
    void soltar_copia() {
        if (copia.imagen) MEMFreeToMappedMemory(copia.imagen);
        copia = {};
        estado = LIBRE;
    }
} // namespace

/* Si hay una captura pedida para esta pantalla, copia el búfer y avisa al servidor. */
/* Llamadas vistas a cada gancho por pantalla: copia, get, mark, swap (para diagnosticar). */
uint32_t llamadas[4][2];

void atender_pedido(const GX2ColorBuffer *buffer, GX2ScanTarget destino) {
    int pantalla = destino == GX2_SCAN_TARGET_TV ? 0 : 1;
    if (!buffer || estado != PEDIDA || pantalla != pantalla_pedida) return;
    OSLockMutex(&cerrojo);
    if (estado == PEDIDA) {
        estado = copiar(buffer, pantalla) ? LISTA : LIBRE;
        OSSignalEvent(&hecho);
    }
    OSUnlockMutex(&cerrojo);
}

/* Camino normal (WHBGfx, la mayoría de juegos): la app copia su búfer al de escaneo. */
DECL_FUNCTION(void, GX2CopyColorBufferToScanBuffer, const GX2ColorBuffer *buffer, GX2ScanTarget destino) {
    ++llamadas[0][destino == GX2_SCAN_TARGET_TV ? 0 : 1];
    atender_pedido(buffer, destino);
    real_GX2CopyColorBufferToScanBuffer(buffer, destino);
}

/* Camino del menú de la Wii U para la tele: dibuja directamente en el búfer de escaneo
 * (lo obtiene con GX2GetCurrentScanBuffer) y avisa con GX2MarkScanBufferCopied. */
GX2ColorBuffer ultimo_scan[2];

DECL_FUNCTION(void, GX2GetCurrentScanBuffer, GX2ScanTarget destino, GX2ColorBuffer *cb) {
    real_GX2GetCurrentScanBuffer(destino, cb);
    ++llamadas[1][destino == GX2_SCAN_TARGET_TV ? 0 : 1];
    if (cb) ultimo_scan[destino == GX2_SCAN_TARGET_TV ? 0 : 1] = *cb;
}

DECL_FUNCTION(void, GX2MarkScanBufferCopied, GX2ScanTarget destino) {
    ++llamadas[2][destino == GX2_SCAN_TARGET_TV ? 0 : 1];
    const GX2ColorBuffer *cb = &ultimo_scan[destino == GX2_SCAN_TARGET_TV ? 0 : 1];
    if (cb->surface.image) atender_pedido(cb, destino);
    real_GX2MarkScanBufferCopied(destino);
}

DECL_FUNCTION(void, GX2SetTVBuffer, void *b, uint32_t t, GX2TVRenderMode modo, GX2SurfaceFormat f, GX2BufferingMode bm) {
    formato_scan[0] = f;
    real_GX2SetTVBuffer(b, t, modo, f, bm);
}

DECL_FUNCTION(void, GX2SetDRCBuffer, void *b, uint32_t t, GX2DrcRenderMode modo, GX2SurfaceFormat f, GX2BufferingMode bm) {
    formato_scan[1] = f;
    real_GX2SetDRCBuffer(b, t, modo, f, bm);
}

WUPS_MUST_REPLACE(GX2CopyColorBufferToScanBuffer, WUPS_LOADER_LIBRARY_GX2, GX2CopyColorBufferToScanBuffer);
/* El QR se pinta aquí, con la GPU ya parada: solo memoria, ninguna orden de GPU (ver superposicion.cpp). */
DECL_FUNCTION(void, GX2SwapScanBuffers) {
    ++llamadas[3][0];
    if (superposicion_visible()) {
        GX2DrawDone();
        for (int i = 0; i < 2; ++i) {
            GX2ColorBuffer scan = {};
            real_GX2GetCurrentScanBuffer(i == 0 ? GX2_SCAN_TARGET_TV : GX2_SCAN_TARGET_DRC, &scan);
            superposicion_en_escaneo(&scan, i);
        }
    }
    real_GX2SwapScanBuffers();
}

WUPS_MUST_REPLACE(GX2SwapScanBuffers, WUPS_LOADER_LIBRARY_GX2, GX2SwapScanBuffers);
WUPS_MUST_REPLACE(GX2GetCurrentScanBuffer, WUPS_LOADER_LIBRARY_GX2, GX2GetCurrentScanBuffer);
WUPS_MUST_REPLACE(GX2MarkScanBufferCopied, WUPS_LOADER_LIBRARY_GX2, GX2MarkScanBufferCopied);
WUPS_MUST_REPLACE(GX2SetTVBuffer, WUPS_LOADER_LIBRARY_GX2, GX2SetTVBuffer);
WUPS_MUST_REPLACE(GX2SetDRCBuffer, WUPS_LOADER_LIBRARY_GX2, GX2SetDRCBuffer);

void captura_iniciar() {
    memset(ultimo_scan, 0, sizeof ultimo_scan);
    OSInitMutex(&cerrojo);
    OSInitEvent(&hecho, FALSE, OS_EVENT_MODE_AUTO);
    estado = LIBRE;
    copia = {};
    for (int i = 0; i < 256; ++i) {
        float v = i / 255.0f;
        float s = v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
        lineal_a_srgb[i] = (uint8_t) lroundf(s * 255.0f);
    }
}

uint8_t *captura_jpeg(int pantalla, int calidad, bool mitad, uint32_t *tamano, const char **motivo) {
    if (superposicion_visible()) {   /* con la pantalla del QR delante, se captura eso, que es lo que se ve */
        Copia c = {};
        c.imagen = superposicion_foto(pantalla, &c.ancho, &c.alto);
        if (c.imagen) {
            c.pitch = c.ancho;
            if (mitad) reducir_a_mitad(&c);
            uint8_t *jpeg = codificar(c, calidad, tamano);
            MEMFreeToMappedMemory(c.imagen);
            if (!jpeg) *motivo = "fallo al comprimir en JPEG";
            return jpeg;
        }
    }
    OSLockMutex(&cerrojo);
    if (estado != LIBRE) soltar_copia();
    pantalla_pedida = pantalla;
    OSResetEvent(&hecho);
    estado = PEDIDA;
    OSUnlockMutex(&cerrojo);

    bool a_tiempo = OSWaitEventWithTimeout(&hecho, 3000000000ULL);   /* en nanosegundos */
    OSLockMutex(&cerrojo);
    if (!a_tiempo || estado != LISTA) {
        *motivo = a_tiempo ? "no se pudo copiar la pantalla" : "la app no mostro ningun fotograma en 3 s";
        soltar_copia();
        OSUnlockMutex(&cerrojo);
        return nullptr;
    }
    Copia c = copia;
    copia = {};
    estado = LIBRE;
    OSUnlockMutex(&cerrojo);

    if (mitad) reducir_a_mitad(&c);
    if (c.a_srgb) convertir_a_srgb(c);
    uint8_t *jpeg = codificar(c, calidad, tamano);
    MEMFreeToMappedMemory(c.imagen);
    if (!jpeg) *motivo = "fallo al comprimir en JPEG";
    return jpeg;
}

void captura_cancelar() { OSSignalEvent(&hecho); }

void captura_liberar(uint8_t *jpeg) {
    tjFree(jpeg);
}

void captura_diagnostico(char *json, uint32_t cap) {
    snprintf(json, cap, "{\"copia\":[%lu,%lu],\"get\":[%lu,%lu],\"mark\":[%lu,%lu],\"swap\":%lu,"
             "\"formato\":[\"0x%x\",\"0x%x\"]}",
             (unsigned long) llamadas[0][0], (unsigned long) llamadas[0][1], (unsigned long) llamadas[1][0],
             (unsigned long) llamadas[1][1], (unsigned long) llamadas[2][0], (unsigned long) llamadas[2][1],
             (unsigned long) llamadas[3][0], (unsigned) formato_scan[0], (unsigned) formato_scan[1]);
}
