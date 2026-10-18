/* Pantalla de emparejamiento: ocupa toda la pantalla, con el QR en el centro, SIN tocar la GPU.
 *
 * Intento de la 0.5.0: pegar una superficie con GX2CopySurfaceEx en cada fotograma desde el gancho de la app.
 * Cambiaba el estado de la GPU de la app y el menú de la Wii U se quedó en negro y luego colgado.
 * Ahora: justo antes de GX2SwapScanBuffers se espera a que la GPU termine (GX2DrawDone) y el procesador copia
 * la pantalla ya preparada en el búfer de escaneo. No se emite ninguna orden a la GPU.
 *
 * La plantilla (fondo, textos, tarjeta) viene hecha de fábrica en JPEG (web/pantalla_qr.py). Al enseñarla se
 * descomprime en un lienzo de 1280x720 y se escriben encima la dirección y el código. La primera vez que se ve
 * cada pantalla (tele, GamePad) se escala a su tamaño y se guarda ya en el formato de su búfer; después, cada
 * fotograma es una copia de memoria. El QR se dibuja directamente a la resolución de cada pantalla, nítido. */
#include "superposicion.h"
#include "comun.h"
#include "idioma.h"
#include "pantalla_qr.h"
#include "qrcodegen.h"

#include <coreinit/cache.h>
#include <coreinit/mutex.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <memory/mappedmemory.h>
#include <turbojpeg.h>

#include <cstdio>
#include <cstring>

namespace {
    constexpr uint32_t MARGEN_QR = 2;        /* módulos blancos alrededor (la tarjeta ya pone más blanco) */
    constexpr uint32_t BLANCO = 0xFBFAF6FF, NEGRO = 0x120B24FF;

    struct Pantalla {
        uint8_t *img = nullptr;
        uint32_t tam = 0, ancho = 0, alto = 0, pitch = 0, swizzle = 0;
        int tile = -1;
    };

    uint32_t *lienzo;                        /* 1280x720 RGBA */
    uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
    int modulos;
    Pantalla cache[2];
    volatile bool visible;
    bool avisado_formato;
    /* Entre quien prepara la pantalla y quien la captura (pueden ser hilos distintos). El gancho de dibujo no lo
     * usa: nunca debe esperar; se protege con `visible` y la pausa de superposicion_mostrar. */
    OSMutex cerrojo;
    struct Cerrar {
        Cerrar() { OSLockMutex(&cerrojo); }
        ~Cerrar() { OSUnlockMutex(&cerrojo); }
    };

    void liberar() {
        for (Pantalla &p : cache) {
            if (p.img) MEMFreeToMappedMemory(p.img);
            p = Pantalla{};
        }
        if (lienzo) MEMFreeToMappedMemory(lienzo);
        lienzo = nullptr;
    }

    /* ---------- lienzo ---------- */

    uint32_t mezclar(uint32_t fondo, uint32_t color, uint32_t a) {
        uint32_t r = 0;
        for (int d = 24; d >= 8; d -= 8) {
            uint32_t f = (fondo >> d) & 0xFF, c = (color >> d) & 0xFF;
            r |= ((c * a + f * (255 - a)) / 255) << d;
        }
        return r | 0xFF;
    }

    struct Letra {
        uint32_t alto, color;
        const char *caracteres;
        const uint16_t *ancho;
        const uint32_t *pos;
        const uint8_t *alfa;
    };
    const Letra LETRA_DIR = {PQR_LETRA_DIR_ALTO, PQR_LETRA_DIR_COLOR, PQR_LETRA_DIR_CARACTERES, PQR_LETRA_DIR_ANCHO, PQR_LETRA_DIR_POS, PQR_LETRA_DIR_ALFA};
    const Letra LETRA_COD = {PQR_LETRA_COD_ALTO, PQR_LETRA_COD_COLOR, PQR_LETRA_COD_CARACTERES, PQR_LETRA_COD_ANCHO, PQR_LETRA_COD_POS, PQR_LETRA_COD_ALFA};

    void escribir(const Letra &l, const char *s, uint32_t x, uint32_t y) {
        for (; *s; ++s) {
            const char *c = strchr(l.caracteres, *s);
            if (!c) continue;
            uint32_t i = c - l.caracteres, w = l.ancho[i];
            const uint8_t *alfa = l.alfa + l.pos[i];
            for (uint32_t j = 0; j < l.alto && y + j < PQR_ALTO; ++j)
                for (uint32_t k = 0; k < w && x + k < PQR_ANCHO; ++k) {
                    uint32_t a = alfa[j * w + k];
                    if (a) lienzo[(y + j) * PQR_ANCHO + x + k] = mezclar(lienzo[(y + j) * PQR_ANCHO + x + k], l.color, a);
                }
            x += w;
        }
    }

    bool preparar_lienzo(const char *codigo, const char *direccion) {
        lienzo = (uint32_t *) MEMAllocFromMappedMemoryEx(PQR_ANCHO * PQR_ALTO * 4, 0x40);
        if (!lienzo) return false;
        tjhandle tj = tjInitDecompress();
        if (!tj) return false;
        const uint8_t *jpg = idioma_es() ? PQR_JPEG_ES : PQR_JPEG_EN;
        unsigned long tam = idioma_es() ? sizeof PQR_JPEG_ES : sizeof PQR_JPEG_EN;
        int r = tjDecompress2(tj, jpg, tam, (unsigned char *) lienzo, PQR_ANCHO, PQR_ANCHO * 4, PQR_ALTO, TJPF_RGBA, 0);
        tjDestroy(tj);
        if (r != 0) return false;
        char texto[64];
        snprintf(texto, sizeof texto, "http://%s", direccion);
        escribir(LETRA_DIR, texto, PQR_DIRECCION[0], PQR_DIRECCION[1]);
        snprintf(texto, sizeof texto, "%.3s %.3s", codigo, codigo + 3);
        escribir(LETRA_COD, texto, PQR_CODIGO[0], PQR_CODIGO[1]);
        return true;
    }

    /* ---------- escalar a cada pantalla ---------- */

    struct Escala {
        uint32_t ox, oy, inv;                 /* desplazamiento y paso en el lienzo (16.16) */
        uint32_t qx, qy, qlado, qmod;         /* QR en píxeles de la pantalla */
    };

    Escala calcular_escala(uint32_t w, uint32_t h) {
        Escala e{};
        uint32_t esc = w * PQR_ALTO < h * PQR_ANCHO ? (w << 16) / PQR_ANCHO : (h << 16) / PQR_ALTO;   /* 16.16 */
        e.ox = (w - ((PQR_ANCHO * esc) >> 16)) / 2;
        e.oy = (h - ((PQR_ALTO * esc) >> 16)) / 2;
        e.inv = (uint32_t) ((uint64_t(1) << 32) / esc);
        uint32_t total = modulos + 2 * MARGEN_QR, zona = (PQR_QR[2] * esc) >> 16;
        e.qmod = zona / total ? zona / total : 1;
        e.qlado = e.qmod * total;
        e.qx = e.ox + (((PQR_QR[0] + PQR_QR[2] / 2) * esc) >> 16) - e.qlado / 2;
        e.qy = e.oy + (((PQR_QR[1] + PQR_QR[3] / 2) * esc) >> 16) - e.qlado / 2;
        return e;
    }

    uint32_t canal(uint32_t c, int d) { return (c >> d) & 0xFF; }

    uint32_t muestra(const Escala &e, uint32_t x, uint32_t y) {
        if (x >= e.qx && x < e.qx + e.qlado && y >= e.qy && y < e.qy + e.qlado) {
            int mx = (int) ((x - e.qx) / e.qmod) - (int) MARGEN_QR, my = (int) ((y - e.qy) / e.qmod) - (int) MARGEN_QR;
            return qrcodegen_getModule(qr, mx, my) ? NEGRO : BLANCO;   /* fuera del QR devuelve false: blanco */
        }
        /* bilineal; fuera del lienzo (pantallas 4:3) se repite el borde */
        int32_t u = (int32_t) (((int64_t) x - e.ox) * e.inv + (e.inv >> 1)) - 32768;
        int32_t v = (int32_t) (((int64_t) y - e.oy) * e.inv + (e.inv >> 1)) - 32768;
        if (u < 0) u = 0;
        if (v < 0) v = 0;
        uint32_t x0 = u >> 16, y0 = v >> 16, fx = (u >> 8) & 0xFF, fy = (v >> 8) & 0xFF;
        if (x0 >= PQR_ANCHO - 1) { x0 = PQR_ANCHO - 2; fx = 255; }
        if (y0 >= PQR_ALTO - 1) { y0 = PQR_ALTO - 2; fy = 255; }
        const uint32_t *p = &lienzo[y0 * PQR_ANCHO + x0];
        uint32_t a = p[0], b = p[1], c = p[PQR_ANCHO], d = p[PQR_ANCHO + 1], r = 0;
        for (int k = 24; k >= 8; k -= 8) {
            uint32_t arriba = canal(a, k) * (256 - fx) + canal(b, k) * fx;
            uint32_t abajo = canal(c, k) * (256 - fx) + canal(d, k) * fx;
            r |= ((arriba * (256 - fy) + abajo * fy) >> 16) << k;
        }
        return r | 0xFF;
    }

    /* Dirección de un bloque de 8x8 píxeles de 32 bits en una superficie 2D_TILED_THIN1 del chip de la Wii U
     * (2 canales, 4 bancos, grupos de 256 bytes): fórmula de AddrLib (R600), la que usa Cemu. Probada en consola. */
    uint32_t bloque_en_mosaico(uint32_t x, uint32_t y, uint32_t pitch, uint32_t swizzle) {
        const uint32_t BANCOS = 4, CANALES = 2;
        uint32_t pipe = ((y >> 3) ^ (x >> 3)) & 1;
        uint32_t bank = (((y / 32) ^ (x >> 3)) & 1) | ((((y / 16) ^ (x >> 4)) & 1) << 1);
        uint32_t giro = ((swizzle >> 8) & 1) + CANALES * ((swizzle >> 9) & 3);
        uint32_t bank_pipe = ((pipe + CANALES * bank) ^ giro) % (CANALES * BANCOS);
        pipe = bank_pipe % CANALES;
        bank = bank_pipe / CANALES;
        const uint32_t MACRO_ANCHO = 8 * BANCOS, MACRO_ALTO = 8 * CANALES, MACRO_BYTES = MACRO_ANCHO * MACRO_ALTO * 4;
        uint32_t macro = ((x / MACRO_ANCHO) + (pitch / MACRO_ANCHO) * (y / MACRO_ALTO)) * MACRO_BYTES;
        uint32_t total = macro >> 3;
        return ((total & ~0xFFu) << 3) | (bank << 9) | (pipe << 8) | (total & 0xFF);
    }

    /* Posición del píxel (x, y) dentro de su bloque de 8x8, para 32 bits por píxel. */
    uint32_t indice_en_bloque(uint32_t x, uint32_t y) {
        return (x & 3) | ((y & 1) << 2) | ((x & 4) << 1) | ((y & 6) << 3);
    }

    bool lineal(int m) { return m == GX2_TILE_MODE_LINEAR_ALIGNED || m == GX2_TILE_MODE_LINEAR_SPECIAL; }

    void rellenar(Pantalla &p, bool mosaico) {
        Escala e = calcular_escala(p.ancho, p.alto);
        if (!mosaico) {
            auto *px = (uint32_t *) p.img;
            for (uint32_t y = 0; y < p.alto; ++y)
                for (uint32_t x = 0; x < p.ancho; ++x) px[y * p.pitch + x] = muestra(e, x, y);
            return;
        }
        for (uint32_t by = 0; by < p.alto; by += 8)
            for (uint32_t bx = 0; bx < p.ancho; bx += 8) {
                uint32_t dir = bloque_en_mosaico(bx, by, p.pitch, p.swizzle);
                if (dir + 256 > p.tam) continue;                  /* nunca fuera de la imagen */
                auto *px = (uint32_t *) (p.img + dir);
                for (uint32_t y = 0; y < 8; ++y)
                    for (uint32_t x = 0; x < 8; ++x)
                        px[indice_en_bloque(x, y)] = muestra(e, bx + x < p.ancho ? bx + x : p.ancho - 1, by + y < p.alto ? by + y : p.alto - 1);
            }
    }

    bool coincide(const Pantalla &p, const GX2Surface &s) {
        return p.img && p.tam == s.imageSize && p.ancho == s.width && p.alto == s.height && p.pitch == s.pitch &&
               p.swizzle == s.swizzle && p.tile == (int) s.tileMode;
    }

    bool preparar_pantalla(Pantalla &p, const GX2Surface &s) {
        if (p.img) MEMFreeToMappedMemory(p.img);
        p = Pantalla{};
        p.img = (uint8_t *) MEMAllocFromMappedMemoryEx(s.imageSize, 0x100);
        if (!p.img) return false;
        p.tam = s.imageSize; p.ancho = s.width; p.alto = s.height; p.pitch = s.pitch; p.swizzle = s.swizzle; p.tile = (int) s.tileMode;
        memset(p.img, 0, p.tam);
        OSTime t0 = OSGetSystemTime();
        rellenar(p, !lineal(p.tile));
        registro("QR: pantalla %ux%u preparada en %u ms", (unsigned) p.ancho, (unsigned) p.alto,
                 (unsigned) OSTicksToMilliseconds(OSGetSystemTime() - t0));
        return true;
    }
} // namespace

void superposicion_iniciar() { OSInitMutex(&cerrojo); }

bool superposicion_mostrar(const char *texto_url, const char *codigo, const char *direccion) {
    Cerrar cierre;
    if (visible || lienzo) {
        visible = false;
        OSSleepTicks(OSMillisecondsToTicks(60));   /* que el gancho termine la copia en curso */
    }
    liberar();
    static uint8_t temporal[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
    if (!qrcodegen_encodeText(texto_url, temporal, qr, qrcodegen_Ecc_MEDIUM, 1, 10, qrcodegen_Mask_AUTO, true))
        return false;
    modulos = qrcodegen_getSize(qr);
    if (!preparar_lienzo(codigo, direccion)) {
        liberar();
        return false;
    }
    visible = true;
    return true;
}

void superposicion_ocultar() { visible = false; }

/* Para las capturas: la pantalla de emparejamiento tal como se ve, en orden lineal, en memoria propia. */
void *superposicion_foto(int pantalla, uint32_t *ancho, uint32_t *alto) {
    Cerrar cierre;
    if (!visible || !lienzo || pantalla < 0 || pantalla > 1) return nullptr;
    const Pantalla &c = cache[pantalla];
    Pantalla p;
    p.ancho = c.img ? c.ancho : (pantalla == 0 ? 1280 : 854);
    p.alto = c.img ? c.alto : (pantalla == 0 ? 720 : 480);
    p.pitch = p.ancho;
    p.tam = p.ancho * p.alto * 4;
    p.img = (uint8_t *) MEMAllocFromMappedMemoryEx(p.tam, 0x40);
    if (!p.img) return nullptr;
    rellenar(p, false);
    *ancho = p.ancho;
    *alto = p.alto;
    return p.img;
}

bool superposicion_visible() { return visible; }

/* Llamada antes de GX2SwapScanBuffers, con la GPU parada: copia la pantalla preparada en el búfer de escaneo. */
void superposicion_en_escaneo(const GX2ColorBuffer *scan, int pantalla) {
    if (!visible || !lienzo || !scan || !scan->surface.image || pantalla < 0 || pantalla > 1) return;
    const GX2Surface &s = scan->surface;
    bool valido = (lineal(s.tileMode) || s.tileMode == GX2_TILE_MODE_TILED_2D_THIN1) && (s.format & 0xff) == 0x1a &&
                  s.width >= 64 && s.height >= 64;
    if (!valido) {
        if (!avisado_formato) registro("QR: pantalla en formato desconocido (tile %d, formato 0x%x): no se dibuja", (int) s.tileMode, (unsigned) s.format);
        avisado_formato = true;
        return;
    }
    Pantalla &p = cache[pantalla];
    if (!coincide(p, s) && !preparar_pantalla(p, s)) return;
    memcpy(s.image, p.img, p.tam);
    DCFlushRange(s.image, p.tam);
}
