#include "servidor.h"
#include "ajustes.h"
#include "comun.h"
#include "captura.h"
#include "dispositivos.h"
#include "firma.h"
#include "http.h"
#include "ordenes.h"
#include "panel_web.h"
#include "sistema.h"
#include "superposicion.h"

#include <arpa/inet.h>
#include <cerrno>
#include <coreinit/cache.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

#define RUTA_PERMITIDOS CARPETA_SD "/permitidos.txt"

namespace {
    constexpr uint32_t PILA = 64 * 1024;
    constexpr int MAX_PERMITIDOS = 8;

    OSThread *hilo;
    uint8_t *pila;
    volatile bool parar;
    int escucha = -1;
    uint32_t permitidos[MAX_PERMITIDOS];   /* solo para la clave del servidor (aparato 0) */
    int n_permitidos;

    struct Entrada {
        const char *metodo, *ruta;
        Orden fn;
        Permiso permiso;
    };
    const Entrada ORDENES[] = {
        {"GET", "/estado", orden_estado, PERMISO_SIEMPRE},
        {"GET", "/info", orden_info, PERMISO_SIEMPRE},
        {"GET", "/registro", orden_registro, PERMISO_SIEMPRE},
        {"GET", "/diagnostico", orden_diagnostico, PERMISO_SIEMPRE},
        {"GET", "/titulos", orden_titulos, PERMISO_SIEMPRE},
        {"GET", "/apps", orden_apps, PERMISO_SIEMPRE},
        {"GET", "/pantalla", orden_pantalla, PERMISO_VER},
        {"POST", "/botones", orden_botones, PERMISO_MANDOS},
        {"POST", "/tocar", orden_tocar, PERMISO_MANDOS},
        {"POST", "/palanca", orden_palanca, PERMISO_MANDOS},
        {"POST", "/abrir", orden_abrir, PERMISO_ABRIR},
        {"POST", "/titulo", orden_titulo, PERMISO_ABRIR},
        {"POST", "/menu", orden_menu, PERMISO_ENERGIA},
        {"POST", "/reiniciar", orden_reiniciar, PERMISO_ENERGIA},
        {"POST", "/apagar", orden_apagar, PERMISO_ENERGIA},
        {"POST", "/clave", orden_clave, PERMISO_ADMIN},
        {"POST", "/aviso", orden_aviso, PERMISO_AVISOS},
        {"POST", "/vibrar", orden_vibrar, PERMISO_AVISOS},
        {"GET", "/archivos", orden_listar, PERMISO_LEER},
        {"GET", "/archivo", orden_bajar, PERMISO_LEER},
        {"PUT", "/archivo", orden_subir, PERMISO_ESCRIBIR},
        {"DELETE", "/archivo", orden_borrar, PERMISO_ESCRIBIR},
        {"POST", "/carpeta", orden_carpeta, PERMISO_ESCRIBIR},
        {"POST", "/olvidar", orden_olvidar, PERMISO_SIEMPRE},
        {"POST", "/invitar", orden_invitar, PERMISO_ADMIN},
    };

    bool ip_permitida(uint32_t ip) {
        if (!n_permitidos) return true;
        for (int i = 0; i < n_permitidos; ++i)
            if (permitidos[i] == ip) return true;
        return false;
    }

    void rechazar(Peticion *p, int codigo, const char *motivo) {
        registro("rechazado %s %s: %s", p->metodo, p->ruta, motivo);
        OSSleepTicks(OSMillisecondsToTicks(500));   /* frena a quien pruebe firmas */
        http_texto(p->sock, codigo, "%s", motivo);
    }

    /* ---------- lo que no necesita firma: el panel y el emparejamiento ---------- */

    void servir_estatico(Peticion *p, const uint8_t *datos, uint32_t largo, const char *tipo, bool gzip) {
        char cab[256];
        int n = snprintf(cab, sizeof cab, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %u\r\n%s"
                         "Cache-Control: no-cache\r\nX-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\n"
                         "Referrer-Policy: no-referrer\r\nConnection: close\r\n\r\n",
                         tipo, (unsigned) largo, gzip ? "Content-Encoding: gzip\r\n" : "");
        if (http_enviar(p->sock, cab, n)) http_enviar(p->sock, datos, largo);
    }

    void hola(Peticion *p) {
        http_texto(p->sock, 200, "{\"titiritero\":\"%s\",\"emparejar\":%s}", TITIRITERO_VERSION,
                   emparejar_abierto() ? "true" : "false");
    }

    void reto(Peticion *p) {
        char json[160];
        char num[12];
        if (!http_param(p, "disp", num, sizeof num) || !num[0] || strspn(num, "0123456789") != strlen(num))
            return http_texto(p->sock, 400, "falta disp (numero del aparato)");
        uint32_t disp = (uint32_t) strtoul(num, nullptr, 10);
        if (disp == ID_SERVIDOR && !ip_permitida(p->ip)) return rechazar(p, 403, "esta IP no esta en permitidos.txt");
        if (!firma_reto(disp, json, sizeof json)) return http_texto(p->sock, 403, "aparato no emparejado");
        http_texto(p->sock, 200, "%s", json);
    }

    void emparejar(Peticion *p) {
        char pub[80], nombre[64] = "", pub_consola[65];
        const char *motivo = "";
        if (!http_param(p, "pub", pub, sizeof pub)) return http_texto(p->sock, 400, "falta pub");
        http_param(p, "nombre", nombre, sizeof nombre);
        if (!emparejar_iniciar(pub, nombre, pub_consola, &motivo)) return rechazar(p, 403, motivo);
        http_texto(p->sock, 200, "{\"pub\":\"%s\"}", pub_consola);
    }

    void emparejar_fin(Peticion *p) {
        char prueba[80], prueba_consola[65];
        const char *motivo = "";
        if (!http_param(p, "prueba", prueba, sizeof prueba)) return http_texto(p->sock, 400, "falta prueba");
        uint32_t id = emparejar_confirmar(prueba, prueba_consola, &motivo);
        if (!id) return rechazar(p, 403, motivo);
        superposicion_ocultar();
        http_texto(p->sock, 200, "{\"id\":%u,\"prueba\":\"%s\"}", (unsigned) id, prueba_consola);
    }

    bool sin_firma(Peticion *p) {
        const bool get = !strcmp(p->metodo, "GET"), post = !strcmp(p->metodo, "POST");
        if (get && !strcmp(p->ruta, "/")) return servir_estatico(p, PANEL_HTML_GZ, sizeof PANEL_HTML_GZ, "text/html; charset=utf-8", true), true;
        if (get && !strcmp(p->ruta, "/icono.svg")) return servir_estatico(p, ICONO_SVG_GZ, sizeof ICONO_SVG_GZ, "image/svg+xml", true), true;
        if (get && !strcmp(p->ruta, "/manifest.webmanifest"))
            return servir_estatico(p, MANIFIESTO_GZ, sizeof MANIFIESTO_GZ, "application/manifest+json", true), true;
        if (get && !strcmp(p->ruta, "/hola")) return hola(p), true;
        if (get && !strcmp(p->ruta, "/reto")) return reto(p), true;
        if (post && !strcmp(p->ruta, "/emparejar")) return emparejar(p), true;
        if (post && !strcmp(p->ruta, "/emparejar/confirmar")) return emparejar_fin(p), true;
        return false;
    }

    /* ---------- órdenes firmadas ---------- */

    /* La petición ya tiene la cabecera entera (ver el bucle). */
    void atender(Peticion &p) {
        const int sock = p.sock;
        if (sin_firma(&p)) return;
        const char *motivo = "";
        uint32_t disp = 0;
        if (!firma_valida(&p, &disp, &motivo)) return rechazar(&p, 403, motivo);
        if (disp == ID_SERVIDOR && !ip_permitida(p.ip)) return rechazar(&p, 403, "esta IP no esta en permitidos.txt");
        p.disp = disp;
        for (const auto &o : ORDENES) {
            if (strcmp(p.metodo, o.metodo) || strcmp(p.ruta, o.ruta)) continue;
            if (o.permiso != PERMISO_SIEMPRE && !ajustes_servidor_activo())
                return http_texto(sock, 403, "control remoto en pausa en la consola (Titiritero)");
            if (!ajustes_permite(o.permiso))
                return http_texto(sock, 403, "desactivado en el menu del plugin de la consola (Titiritero)");
            ajustes_actividad(disp, p.ruta);
            return o.fn(&p);
        }
        http_texto(sock, 404, "orden desconocida");
    }

    /* La clave del servidor (opcional): solo existe si alguien usa la orden `titiritero` desde un PC. */
    void cargar_clave() {
        char clave[80] = "";
        FILE *f = fopen(RUTA_CLAVE, "r");
        if (f) {
            if (fgets(clave, sizeof clave, f)) clave[strcspn(clave, "\r\n ")] = 0;
            fclose(f);
        }
        dispositivos_poner_clave_servidor(clave);
        firma_reiniciar();
    }

    void ejecutar_despues() {
        switch (ordenes_pendiente()) {
            case DESPUES_MENU: sistema_menu(); break;
            case DESPUES_REINICIAR: sistema_reiniciar(); break;
            case DESPUES_APAGAR: sistema_apagar(); break;
            case DESPUES_CLAVE: cargar_clave(); break;   /* invalida todas las sesiones */
            default: break;
        }
    }

    /* Una IP por línea; si el archivo no existe la clave del servidor vale desde cualquier IP. */
    void cargar_permitidos() {
        n_permitidos = 0;
        FILE *f = fopen(RUTA_PERMITIDOS, "r");
        if (!f) return;
        char linea[64];
        while (n_permitidos < MAX_PERMITIDOS && fgets(linea, sizeof linea, f)) {
            linea[strcspn(linea, "\r\n #")] = 0;
            in_addr a;
            if (linea[0] && inet_aton(linea, &a)) permitidos[n_permitidos++] = a.s_addr;
        }
        fclose(f);
    }

    int abrir_escucha() {
        int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s < 0) return -1;
        int si = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &si, sizeof si);
        sockaddr_in dir = {};
        dir.sin_family = AF_INET;
        dir.sin_port = htons(TITIRITERO_PUERTO);
        dir.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s, (sockaddr *) &dir, sizeof dir) < 0 || listen(s, 16) < 0) {
            close(s);
            return -1;
        }
        return s;
    }

    /* Conexiones abiertas. Se vigilan todas a la vez con select, así una lenta (a propósito o no) no frena a las
     * demás:
     *  - LEYENDO: la cabecera aún está llegando (máximo 3 s).
     *  - CERRANDO: ya se respondió y se espera a que el cliente cierre (máximo 2 s). Que cierre primero el cliente
     *    importa: el lado que cierra primero guarda la conexión un rato (TIME_WAIT) y la Wii U se queda sin huecos
     *    tras unas 380 conexiones seguidas (medido: las nuevas dejaban de responder).
     * Si la tabla se llena, sale primero una que ya esté cerrando, luego una que no haya mandado nada. */
    constexpr int MAX_CONEXIONES = 16;
    constexpr uint32_t ESPERA_CABECERA_MS = 3000, ESPERA_CIERRE_MS = 2000;
    enum EstadoConexion { LIBRE, LEYENDO, CERRANDO };
    struct Conexion {
        Peticion p;
        OSTime desde;
        EstadoConexion estado;
    };
    Conexion conexiones[MAX_CONEXIONES];

    void cerrar(Conexion &x) {
        close(x.p.sock);
        x.estado = LIBRE;
    }

    /* Prioridad para salir de la tabla si está llena: menor = sale antes. */
    int prioridad(const Conexion &x) { return x.estado == CERRANDO ? 0 : !x.p.leidos ? 1 : 2; }

    Conexion &hueco_para_nueva() {
        Conexion *h = nullptr;
        for (auto &x : conexiones)
            if (x.estado == LIBRE) return x;
        for (auto &x : conexiones)
            if (!h || prioridad(x) < prioridad(*h) || (prioridad(x) == prioridad(*h) && x.desde < h->desde)) h = &x;
        cerrar(*h);
        return *h;
    }

    void aceptar() {
        sockaddr_in cliente = {};
        socklen_t largo = sizeof cliente;
        int c = accept(escucha, (sockaddr *) &cliente, &largo);
        if (c < 0) {
            static OSTime aviso;
            if (OSTicksToSeconds(OSGetTime() - aviso) > 10) registro("accept fallo (errno %d)", errno), aviso = OSGetTime();
            return;
        }
        if (c >= FD_SETSIZE) {   /* select no puede vigilarlo */
            registro("socket %d fuera de FD_SETSIZE", c);
            close(c);
            return;
        }
        Conexion &x = hueco_para_nueva();
        memset(&x.p, 0, sizeof x.p);
        x.p.sock = c;
        x.p.ip = cliente.sin_addr.s_addr;
        x.desde = OSGetTime();
        x.estado = LEYENDO;
    }

    void responder(Conexion &x) {
        switch (http_leer_parte(&x.p)) {
            case HTTP_FALTA: return;
            case HTTP_MAL: http_texto(x.p.sock, 400, "peticion no valida"); break;
            case HTTP_LISTA: {
                OSTime t0 = OSGetTime();
                atender(x.p);
                uint32_t ms = (uint32_t) OSTicksToMilliseconds(OSGetTime() - t0);
                if (ms > 1500) registro("lenta: %s %s (%u ms)", x.p.metodo, x.p.ruta, (unsigned) ms);
                break;
            }
        }
        x.estado = CERRANDO;   /* sin shutdown: con SHUT_RD, recv daría 0 al momento y cerraríamos primero */
        x.desde = OSGetTime();
        ejecutar_despues();
    }

    /* En CERRANDO: se descarta lo que llegue; con 0 bytes (el cliente cerró) se cierra sin TIME_WAIT aquí. */
    void esperar_cierre(Conexion &x) {
        static char basura[512];
        if (recv(x.p.sock, basura, sizeof basura, 0) <= 0) cerrar(x);
    }

    void vuelta() {
        fd_set lectura;
        FD_ZERO(&lectura);
        FD_SET(escucha, &lectura);
        int mayor = escucha;
        OSTime ahora = OSGetTime();
        for (auto &x : conexiones) {
            if (x.estado == LIBRE) continue;
            uint32_t limite = x.estado == LEYENDO ? ESPERA_CABECERA_MS : ESPERA_CIERRE_MS;
            if (OSTicksToMilliseconds(ahora - x.desde) > limite) {
                cerrar(x);
                continue;
            }
            FD_SET(x.p.sock, &lectura);
            if (x.p.sock > mayor) mayor = x.p.sock;
        }
        timeval t = {0, 200000};   /* nunca bloquearse: al cerrar la app, servidor_parar espera a este hilo */
        if (select(mayor + 1, &lectura, nullptr, nullptr, &t) <= 0) return;
        for (auto &x : conexiones) {
            if (x.estado == LIBRE || !FD_ISSET(x.p.sock, &lectura)) continue;
            if (x.estado == LEYENDO) responder(x);
            else esperar_cierre(x);
        }
        if (FD_ISSET(escucha, &lectura)) aceptar();
    }

    int bucle(int, const char **) {
        while (!parar) {
            if (superposicion_visible() && !emparejar_abierto()) superposicion_ocultar();   /* el código caducó */
            if (escucha < 0) escucha = abrir_escucha();
            if (escucha < 0) {
                OSSleepTicks(OSMillisecondsToTicks(200));
                continue;
            }
            vuelta();
        }
        for (auto &x : conexiones)
            if (x.estado != LIBRE) cerrar(x);
        return 0;
    }
} // namespace

void servidor_arrancar() {
    if (hilo) return;   /* ya está en marcha */
    cargar_clave();
    cargar_permitidos();
    parar = false;
    ordenes_programar(DESPUES_NADA);
    hilo = (OSThread *) memalign(16, sizeof(OSThread));
    pila = (uint8_t *) memalign(16, PILA);
    if (!hilo || !pila) return registro("sin memoria para el hilo del servidor");
    if (!OSCreateThread(hilo, bucle, 0, nullptr, pila + PILA, PILA, 20, OS_THREAD_ATTRIB_AFFINITY_CPU2))
        return registro("no se pudo crear el hilo del servidor");
    OSSetThreadName(hilo, "Titiritero");
    OSResumeThread(hilo);
}

bool servidor_parando() { return parar; }

void servidor_parar() {
    if (!hilo) return;
    parar = true;
    OSMemoryBarrier();
    captura_cancelar();   /* una captura esperando un fotograma que ya no llegará */
    OSJoinThread(hilo, nullptr);   /* sale en <200 ms: el bucle mira `parar` entre esperas */
    if (escucha >= 0) {
        close(escucha);
        escucha = -1;
    }
    free(hilo);
    free(pila);
    hilo = nullptr;
    pila = nullptr;
}
