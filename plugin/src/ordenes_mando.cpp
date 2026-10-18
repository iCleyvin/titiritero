/* Órdenes de pantalla y GamePad. */
#include "captura.h"
#include "ajustes.h"
#include "comun.h"
#include "mando.h"
#include "ordenes.h"

#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <cstdlib>
#include <cstring>

namespace {
    uint32_t duracion(const Peticion *p, long defecto) {
        return (uint32_t) http_param_num(p, "ms", defecto, 16, 10000);
    }

    float eje(const Peticion *p, const char *nombre) {
        char v[16] = "0";
        http_param(p, nombre, v, sizeof v);
        float f = strtof(v, nullptr);
        return f < -1 ? -1 : f > 1 ? 1 : f;
    }
} // namespace

/* Límite de imágenes por segundo del menú (es lo que deja fluido el juego). Devuelve cuántos ms faltan para la
 * siguiente; el servidor no espera aquí, porque mientras esperase no atendería a nadie más. */
static uint32_t falta_para_turno() {
    static OSTime ultima;
    OSTime minimo = OSMillisecondsToTicks(1000 / ajustes_fps());
    OSTime ahora = OSGetSystemTime();
    if (ultima && ahora - ultima < minimo) return (uint32_t) OSTicksToMilliseconds(minimo - (ahora - ultima)) + 1;
    ultima = ahora;
    return 0;
}

void orden_pantalla(Peticion *p) {
    if (uint32_t espera = falta_para_turno()) return http_texto(p->sock, 429, "{\"espera\":%u}", (unsigned) espera);
    char de[12] = "tv";
    http_param(p, "de", de, sizeof de);
    int pantalla = !strcmp(de, "gamepad") ? 1 : 0;
    int calidad = (int) http_param_num(p, "calidad", 85, 10, 100);
    bool mitad = http_param_num(p, "mitad", 0, 0, 1) == 1;
    uint32_t largo = 0;
    const char *motivo = "";
    uint8_t *jpeg = captura_jpeg(pantalla, calidad, mitad, &largo, &motivo);
    if (!jpeg) return http_texto(p->sock, 500, "%s", motivo);
    http_responder(p->sock, 200, "image/jpeg", jpeg, largo);
    captura_liberar(jpeg);
}

void orden_diagnostico(Peticion *p) {
    char json[256];
    captura_diagnostico(json, sizeof json);
    http_texto(p->sock, 200, "%s", json);
}

void orden_botones(Peticion *p) {
    char nombres[96];
    uint32_t mascara;
    if (!http_param(p, "b", nombres, sizeof nombres) || !mando_botones(nombres, &mascara))
        return http_texto(p->sock, 400, "botones no validos (ej.: b=A o b=ZL,ARRIBA)");
    mando_pulsar(mascara, duracion(p, 120));
    http_texto(p->sock, 200, "pulsado");
}

void orden_tocar(Peticion *p) {
    int x = (int) http_param_num(p, "x", -1, -1, 853);
    int y = (int) http_param_num(p, "y", -1, -1, 479);
    if (x < 0 || y < 0) return http_texto(p->sock, 400, "faltan x e y (0..853, 0..479)");
    if (!mando_tocar(x, y, duracion(p, 120))) return http_texto(p->sock, 500, "no se pudo calcular el punto del panel");
    http_texto(p->sock, 200, "tocado");
}

void orden_palanca(Peticion *p) {
    char cual[8] = "izq";
    http_param(p, "cual", cual, sizeof cual);
    mando_palanca(!strcmp(cual, "der") ? 1 : 0, eje(p, "x"), eje(p, "y"), duracion(p, 200));
    http_texto(p->sock, 200, "movida");
}

void orden_vibrar(Peticion *p) {
    mando_vibrar(duracion(p, 800));
    http_texto(p->sock, 200, "vibrado");
}
