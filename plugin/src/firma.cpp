#include "firma.h"
#include "comun.h"
#include "dispositivos.h"
#include "entropia.h"

#include <coreinit/time.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

struct Sha256 {
    mbedtls_sha256_context ctx;
};

namespace {
    constexpr int MAX_SESIONES = 24;
    constexpr uint32_t HEX_SHA = 64;

    struct Sesion {
        uint32_t id;
        uint32_t disp;        /* aparato al que pertenece */
        char reto[HEX_SHA + 1];
        uint64_t contador;    /* último aceptado */
        OSTime uso;
    };

    Sesion sesiones[MAX_SESIONES];
    uint32_t siguiente_id;
    OSTime arranque;

    void a_hex(const uint8_t *bin, uint32_t n, char *hex) {
        static const char D[] = "0123456789abcdef";
        for (uint32_t i = 0; i < n; ++i) hex[2 * i] = D[bin[i] >> 4], hex[2 * i + 1] = D[bin[i] & 15];
        hex[2 * n] = 0;
    }

    void hmac_hex(const char *clave, const char *mensaje, uint32_t largo, char *hex) {
        uint8_t mac[32];
        mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t *) clave, strlen(clave),
                        (const uint8_t *) mensaje, largo, mac);
        a_hex(mac, sizeof mac, hex);
    }

    bool iguales(const char *a, const char *b) {
        size_t la = strlen(a), lb = strlen(b);
        unsigned dif = la ^ lb;
        for (size_t i = 0; i < la && i < lb; ++i) dif |= (unsigned char) a[i] ^ (unsigned char) b[i];
        return dif == 0;
    }

    Sesion *buscar(uint32_t id) {
        for (auto &s : sesiones)
            if (s.id && s.id == id) return &s;
        return nullptr;
    }

    /* Hueco para una sesión nueva. Pedir un reto no exige firma, así que cualquiera puede pedir muchos:
     * se descartan antes las sesiones que nunca firmaron nada, y solo si no queda ninguna, la menos usada. */
    Sesion *hueco() {
        Sesion *sin_usar = nullptr, *viejo = &sesiones[0];
        for (auto &s : sesiones) {
            if (!s.id) return &s;
            if (!s.contador && (!sin_usar || s.uso < sin_usar->uso)) sin_usar = &s;
            if (s.uso < viejo->uso) viejo = &s;
        }
        return sin_usar ? sin_usar : viejo;
    }

    /* Texto que se firma: reto, contador, método, ruta, consulta y hash del cuerpo, separados por \n. */
    int canonico(char *dst, uint32_t cap, const Sesion *s, const char *contador, const Peticion *p, const char *cuerpo) {
        return snprintf(dst, cap, "%s\n%s\n%s\n%s\n%s\n%s", s->reto, contador, p->metodo, p->ruta, p->consulta, cuerpo);
    }
} // namespace

void firma_reiniciar() {
    memset(sesiones, 0, sizeof sesiones);
    if (!arranque) arranque = OSGetTime();
    if (!siguiente_id) siguiente_id = (uint32_t) (OSGetTime() & 0xFFFF) + 1;
}

bool firma_reto(uint32_t disp, char *json, uint32_t cap) {
    const char *clave = dispositivos_clave(disp);
    if (!clave) return false;
    Sesion *s = hueco();
    memset(s, 0, sizeof *s);
    s->id = siguiente_id++;
    s->disp = disp;
    s->uso = OSGetTime();
    uint8_t azar[16] = {};
    char azar_hex[33];
    entropia_bytes(azar, sizeof azar);   /* si aún no hay entropía, el HMAC con la clave basta para que no se adivine */
    a_hex(azar, sizeof azar, azar_hex);
    char semilla[160];
    int n = snprintf(semilla, sizeof semilla, "reto\n%llu\n%llu\n%u\n%s", (unsigned long long) arranque,
                     (unsigned long long) s->uso, (unsigned) s->id, azar_hex);
    hmac_hex(clave, semilla, n, s->reto);
    snprintf(json, cap, "{\"sesion\":%u,\"reto\":\"%s\"}", (unsigned) s->id, s->reto);
    return true;
}

bool firma_valida(const Peticion *p, uint32_t *disp, const char **motivo) {
    char sesion[16], contador[24], cuerpo[HEX_SHA + 2], dada[HEX_SHA + 2];
    if (!http_cabecera(p, "X-Sesion", sesion, sizeof sesion) || !http_cabecera(p, "X-Contador", contador, sizeof contador) ||
        !http_cabecera(p, "X-Cuerpo", cuerpo, sizeof cuerpo) || !http_cabecera(p, "X-Firma", dada, sizeof dada))
        return *motivo = "orden sin firmar (pide antes GET /reto)", false;
    Sesion *s = buscar((uint32_t) strtoul(sesion, nullptr, 10));
    if (!s) return *motivo = "sesion caducada: pide otro reto", false;
    const char *clave = dispositivos_clave(s->disp);
    if (!clave) return *motivo = "este aparato ya no esta emparejado", false;
    uint64_t n = strtoull(contador, nullptr, 10);
    static char texto[sizeof p->ruta + sizeof p->consulta + 256];
    int largo = canonico(texto, sizeof texto, s, contador, p, cuerpo);
    char esperada[HEX_SHA + 1];
    hmac_hex(clave, texto, largo, esperada);
    if (!iguales(esperada, dada)) return *motivo = "firma incorrecta", false;
    if (n <= s->contador) return *motivo = "orden repetida (contador viejo)", false;
    s->contador = n;
    s->uso = OSGetTime();
    *disp = s->disp;
    return true;
}

Sha256 *sha256_nuevo() {
    auto *s = (Sha256 *) malloc(sizeof(Sha256));
    if (!s) return nullptr;
    mbedtls_sha256_init(&s->ctx);
    mbedtls_sha256_starts_ret(&s->ctx, 0);
    return s;
}

void sha256_sumar(Sha256 *s, const void *datos, uint32_t largo) {
    if (s) mbedtls_sha256_update_ret(&s->ctx, (const uint8_t *) datos, largo);
}

void sha256_fin(Sha256 *s, char *hex) {
    uint8_t h[32] = {};
    if (s) {
        mbedtls_sha256_finish_ret(&s->ctx, h);
        mbedtls_sha256_free(&s->ctx);
        free(s);
    }
    a_hex(h, sizeof h, hex);
}

bool firma_descifrar_clave(const Peticion *p, char *nueva, uint32_t cap) {
    char sesion[16], contador[24], cifrado[2 * 48 + 2];
    if (cap < 49 || !http_cabecera(p, "X-Sesion", sesion, sizeof sesion) ||
        !http_cabecera(p, "X-Contador", contador, sizeof contador) || !http_param(p, "c", cifrado, sizeof cifrado) ||
        strlen(cifrado) != 96)
        return false;
    Sesion *s = buscar((uint32_t) strtoul(sesion, nullptr, 10));
    const char *clave = s ? dispositivos_clave(s->disp) : nullptr;
    if (!s || s->disp != ID_SERVIDOR || !clave) return false;   /* solo la clave del servidor se cambia así */
    uint8_t flujo[64];
    for (int bloque = 0; bloque < 2; ++bloque) {
        char semilla[160], hex[HEX_SHA + 1];
        int n = snprintf(semilla, sizeof semilla, "rotar\n%s\n%s\n%d", s->reto, contador, bloque + 1);
        hmac_hex(clave, semilla, n, hex);
        for (int i = 0; i < 32; ++i) {
            char par[3] = {hex[2 * i], hex[2 * i + 1], 0};
            flujo[bloque * 32 + i] = (uint8_t) strtoul(par, nullptr, 16);
        }
    }
    for (int i = 0; i < 48; ++i) {
        char par[3] = {cifrado[2 * i], cifrado[2 * i + 1], 0};
        char c = (char) ((uint8_t) strtoul(par, nullptr, 16) ^ flujo[i]);
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        nueva[i] = c;
    }
    nueva[48] = 0;
    return true;
}
