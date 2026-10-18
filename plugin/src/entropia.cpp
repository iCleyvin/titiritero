#include "entropia.h"
#include "comun.h"

#include <coreinit/mutex.h>
#include <coreinit/time.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/sha256.h>
#include <wups/storage.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {
    constexpr uint32_t LECTURAS_MINIMAS = 256;   /* ~4 s de GamePad a 60 lecturas por segundo */

    OSMutex cerrojo;
    mbedtls_sha256_context reserva;
    mbedtls_ctr_drbg_context drbg;
    uint32_t lecturas;
    bool sembrado;

    void sumar_hora() {
        OSTime t[2] = {OSGetSystemTime(), OSGetTime()};
        mbedtls_sha256_update_ret(&reserva, (const uint8_t *) t, sizeof t);
    }

    /* Entropía para el DRBG: lo acumulado en la reserva (y se reinicia con su propio resultado). */
    int fuente(void *, unsigned char *destino, size_t largo) {
        uint8_t h[32];
        sumar_hora();
        mbedtls_sha256_finish_ret(&reserva, h);
        mbedtls_sha256_starts_ret(&reserva, 0);
        mbedtls_sha256_update_ret(&reserva, h, sizeof h);
        for (size_t i = 0; i < largo; ++i) destino[i] = h[i % sizeof h];
        return 0;
    }

    void cargar_semilla() {
        std::string semilla;
        if (WUPSStorageAPI::Get<std::string>("semilla", semilla) == WUPS_STORAGE_ERROR_SUCCESS)
            mbedtls_sha256_update_ret(&reserva, (const uint8_t *) semilla.data(), semilla.size());
    }

    bool sembrar() {
        if (sembrado) return true;
        if (lecturas < LECTURAS_MINIMAS) return false;
        static const char ETIQUETA[] = "titiritero-drbg";
        sembrado = mbedtls_ctr_drbg_seed(&drbg, fuente, nullptr, (const uint8_t *) ETIQUETA, sizeof ETIQUETA) == 0;
        mbedtls_ctr_drbg_set_prediction_resistance(&drbg, MBEDTLS_CTR_DRBG_PR_ON);   /* siempre resiembra */
        return sembrado;
    }
} // namespace

void entropia_iniciar() {
    OSInitMutex(&cerrojo);
    mbedtls_sha256_init(&reserva);
    mbedtls_sha256_starts_ret(&reserva, 0);
    mbedtls_ctr_drbg_init(&drbg);
    sumar_hora();
    cargar_semilla();
}

void entropia_sumar(const void *datos, uint32_t largo) {
    if (!OSTryLockMutex(&cerrojo)) return;   /* nunca frenar el hilo del juego */
    sumar_hora();
    mbedtls_sha256_update_ret(&reserva, (const uint8_t *) datos, largo);
    ++lecturas;
    OSUnlockMutex(&cerrojo);
}

bool entropia_bytes(uint8_t *destino, uint32_t largo) {
    OSLockMutex(&cerrojo);
    bool ok = sembrar() && mbedtls_ctr_drbg_random(&drbg, destino, largo) == 0;
    OSUnlockMutex(&cerrojo);
    return ok;
}

int entropia_rng(void *, unsigned char *destino, size_t largo) {
    return entropia_bytes(destino, (uint32_t) largo) ? 0 : -1;
}

void entropia_guardar_semilla() {
    uint8_t s[32];
    if (!entropia_bytes(s, sizeof s)) return;
    char hex[65];
    for (int i = 0; i < 32; ++i) snprintf(hex + 2 * i, 3, "%02x", s[i]);
    WUPSStorageAPI::Store<std::string>("semilla", hex);
    WUPSStorageAPI::SaveStorage();
}
