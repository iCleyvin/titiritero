#include "mando.h"
#include "comun.h"
#include "superposicion.h"
#include "entropia.h"

#include <coreinit/mutex.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <vpad/input.h>
#include <wups.h>

#include <cstring>
#include <strings.h>

namespace {
    struct Inyeccion {
        uint32_t botones;
        bool tocando;
        uint16_t tx, ty;          /* en coordenadas crudas del panel táctil */
        bool palanca[2];
        VPADVec2D eje[2];
    };

    Inyeccion actual;
    uint32_t previa;              /* botones inyectados en la lectura anterior */
    volatile uint8_t bateria = 0xFF;   /* 0..6 según VPAD; 0xFF = aún no leída */
    volatile OSTime ultima_lectura;
    OSMutex cerrojo;

    struct Nombre {
        const char *texto;
        uint32_t bit;
    };
    const Nombre NOMBRES[] = {
        {"A", VPAD_BUTTON_A}, {"B", VPAD_BUTTON_B}, {"X", VPAD_BUTTON_X}, {"Y", VPAD_BUTTON_Y},
        {"ARRIBA", VPAD_BUTTON_UP}, {"ABAJO", VPAD_BUTTON_DOWN},
        {"IZQUIERDA", VPAD_BUTTON_LEFT}, {"DERECHA", VPAD_BUTTON_RIGHT},
        {"UP", VPAD_BUTTON_UP}, {"DOWN", VPAD_BUTTON_DOWN}, {"LEFT", VPAD_BUTTON_LEFT}, {"RIGHT", VPAD_BUTTON_RIGHT},
        {"L", VPAD_BUTTON_L}, {"R", VPAD_BUTTON_R}, {"ZL", VPAD_BUTTON_ZL}, {"ZR", VPAD_BUTTON_ZR},
        {"MAS", VPAD_BUTTON_PLUS}, {"MENOS", VPAD_BUTTON_MINUS}, {"PLUS", VPAD_BUTTON_PLUS}, {"MINUS", VPAD_BUTTON_MINUS},
        {"HOME", VPAD_BUTTON_HOME}, {"STICKL", VPAD_BUTTON_STICK_L}, {"STICKR", VPAD_BUTTON_STICK_R},
    };

    void bloquear() { OSLockMutex(&cerrojo); }

    void desbloquear() { OSUnlockMutex(&cerrojo); }

    void dormir_ms(uint32_t ms) { OSSleepTicks(OSMillisecondsToTicks(ms)); }

    void aplicar_tactil(VPADTouchData *t, const Inyeccion &in) {
        t->x = in.tx;
        t->y = in.ty;
        t->touched = 1;
        t->validity = VPAD_VALID;
    }

    void aplicar(VPADStatus *s, const Inyeccion &in) {
        s->hold |= in.botones;
        if (in.tocando) {
            aplicar_tactil(&s->tpNormal, in);
            aplicar_tactil(&s->tpFiltered1, in);
            aplicar_tactil(&s->tpFiltered2, in);
        }
        if (in.palanca[0]) s->leftStick = in.eje[0];
        if (in.palanca[1]) s->rightStick = in.eje[1];
    }

    /* Punto calibrado (854x480) que corresponde a un punto crudo del panel. */
    void calibrar(uint16_t rx, uint16_t ry, float *cx, float *cy) {
        VPADTouchData crudo = {rx, ry, 1, VPAD_VALID}, cal = {};
        VPADGetTPCalibratedPointEx(VPAD_CHAN_0, VPAD_TP_854X480, &cal, &crudo);
        *cx = cal.x;
        *cy = cal.y;
    }

    /* La calibración es lineal: con dos puntos se invierte. */
    bool crudo_desde_pantalla(int x, int y, uint16_t *rx, uint16_t *ry) {
        const uint16_t A = 1000, B = 3000;
        float ax, ay, bx, by;
        calibrar(A, A, &ax, &ay);
        calibrar(B, B, &bx, &by);
        if (bx == ax || by == ay) return false;
        float fx = A + (x - ax) * (B - A) / (bx - ax);
        float fy = A + (y - ay) * (B - A) / (by - ay);
        if (fx < 0 || fy < 0 || fx > 4095 || fy > 4095) return false;
        *rx = (uint16_t) fx;
        *ry = (uint16_t) fy;
        return true;
    }

    void inyectar(const Inyeccion &in, uint32_t ms) {
        bloquear();
        actual = in;
        desbloquear();
        dormir_ms(ms);
        bloquear();
        memset(&actual, 0, sizeof actual);
        desbloquear();
        dormir_ms(60);   /* deja pasar unas lecturas para que la app vea que se soltó */
    }
    /* Con la pantalla del QR delante, lo que se pulse no llega a lo de detrás; B la cierra. */
    void tapar_mando(VPADStatus *b, int32_t n) {
        if (b[0].trigger & VPAD_BUTTON_B) superposicion_ocultar();
        for (int32_t i = 0; i < n; ++i) {
            b[i].hold = b[i].trigger = b[i].release = 0;
            b[i].tpNormal.touched = b[i].tpFiltered1.touched = b[i].tpFiltered2.touched = 0;
            b[i].leftStick = b[i].rightStick = VPADVec2D{0, 0};
        }
    }
} // namespace

DECL_FUNCTION(int32_t, VPADRead, VPADChan chan, VPADStatus *buffers, uint32_t count, VPADReadError *error) {
    int32_t n = real_VPADRead(chan, buffers, count, error);
    if (chan == VPAD_CHAN_0 && buffers) entropia_sumar(buffers, sizeof(VPADStatus));   /* ruido de los sensores */
    if (chan != VPAD_CHAN_0 || n <= 0 || !buffers) return n;
    bateria = buffers[0].battery;
    if (superposicion_visible()) tapar_mando(buffers, n);
    ultima_lectura = OSGetSystemTime();
    bloquear();
    Inyeccion in = actual;
    desbloquear();
    for (int32_t i = 0; i < n; ++i) aplicar(&buffers[i], in);
    buffers[0].trigger |= in.botones & ~previa;
    buffers[0].release |= previa & ~in.botones;
    previa = in.botones;
    return n;
}

WUPS_MUST_REPLACE(VPADRead, WUPS_LOADER_LIBRARY_VPAD, VPADRead);

void mando_iniciar() {
    OSInitMutex(&cerrojo);
    memset(&actual, 0, sizeof actual);
    previa = 0;
}

bool mando_botones(const char *nombres, uint32_t *mascara) {
    char copia[128];
    strncpy(copia, nombres, sizeof copia - 1);
    copia[sizeof copia - 1] = 0;
    *mascara = 0;
    char *resto = copia;
    for (char *tok = strsep(&resto, ","); tok; tok = strsep(&resto, ",")) {
        uint32_t bit = 0;
        for (const auto &n : NOMBRES)
            if (!strcasecmp(tok, n.texto)) bit = n.bit;
        if (!bit) return false;
        *mascara |= bit;
    }
    return *mascara != 0;
}

void mando_pulsar(uint32_t mascara, uint32_t ms) {
    Inyeccion in = {};
    in.botones = mascara;
    inyectar(in, ms);
}

bool mando_tocar(int x, int y, uint32_t ms) {
    Inyeccion in = {};
    if (!crudo_desde_pantalla(x, y, &in.tx, &in.ty)) return false;
    in.tocando = true;
    inyectar(in, ms);
    return true;
}

void mando_palanca(int cual, float x, float y, uint32_t ms) {
    Inyeccion in = {};
    in.palanca[cual] = true;
    in.eje[cual] = {x, y};
    inyectar(in, ms);
}

int mando_bateria() {
    if (!ultima_lectura || OSTicksToSeconds(OSGetSystemTime() - ultima_lectura) > 5) return -1;
    return bateria <= 6 ? bateria : -1;
}

/* El motor sigue un patrón de bits; con todos a 1 vibra sin pausa hasta pararlo. */
void mando_vibrar(uint32_t ms) {
    static uint8_t patron[15];
    memset(patron, 0xFF, sizeof patron);
    for (uint32_t hecho = 0; hecho < ms; hecho += 250) {
        VPADControlMotor(VPAD_CHAN_0, patron, sizeof patron * 8);
        dormir_ms(ms - hecho < 250 ? ms - hecho : 250);
    }
    VPADStopMotor(VPAD_CHAN_0);
}
