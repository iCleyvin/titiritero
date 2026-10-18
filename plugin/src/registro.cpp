#include "comun.h"

#include <coreinit/mutex.h>
#include <coreinit/time.h>
#include <notifications/notifications.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {
    constexpr uint32_t CAPACIDAD = 16 * 1024;
    char anillo[CAPACIDAD];
    uint32_t escrito;   /* total de bytes escritos desde el arranque */
    OSMutex cerrojo;
    bool listo;

    void asegurar_cerrojo() {
        if (!listo) {
            OSInitMutex(&cerrojo);
            listo = true;
        }
    }
} // namespace

void registro(const char *fmt, ...) {
    char linea[256];
    OSCalendarTime t;
    OSTicksToCalendarTime(OSGetTime(), &t);
    int n = snprintf(linea, sizeof linea, "%02d:%02d:%02d ", t.tm_hour, t.tm_min, t.tm_sec);
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(linea + n, sizeof linea - n - 1, fmt, ap);
    va_end(ap);
    if (n > (int) sizeof linea - 2) n = sizeof linea - 2;
    linea[n++] = '\n';

    asegurar_cerrojo();
    OSLockMutex(&cerrojo);
    for (int i = 0; i < n; ++i) anillo[(escrito + i) % CAPACIDAD] = linea[i];
    escrito += n;
    OSUnlockMutex(&cerrojo);
}

uint32_t registro_copiar(char *destino, uint32_t capacidad) {
    asegurar_cerrojo();
    OSLockMutex(&cerrojo);
    uint32_t total = escrito < CAPACIDAD ? escrito : CAPACIDAD;
    if (total > capacidad) total = capacidad;
    uint32_t inicio = escrito - total;
    for (uint32_t i = 0; i < total; ++i) destino[i] = anillo[(inicio + i) % CAPACIDAD];
    OSUnlockMutex(&cerrojo);
    return total;
}

void aviso(const char *texto) {
    char linea[160];
    snprintf(linea, sizeof linea, "Titiritero: %s", texto);
    NotificationModule_AddInfoNotification(linea);
}
