/* Órdenes de sistema: estado, información, listas de juegos y apps, abrir, menú, reinicio, apagado. */
#include "ajustes.h"
#include "comun.h"
#include "dispositivos.h"
#include "firma.h"
#include "mando.h"
#include "ordenes.h"
#include "sistema.h"
#include "superposicion.h"
#include "titulos.h"

#include <coreinit/mcp.h>
#include <coreinit/time.h>
#include <coreinit/title.h>
#include <notifications/notifications.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace {
    Despues pendiente = DESPUES_NADA;

    unsigned long long segundos_encendida() {
        return (unsigned long long) OSTicksToSeconds(OSGetSystemTime());
    }

    void sistema_json(Texto *t) {
        int mcp = MCP_Open();
        if (mcp < 0) return;
        MCPSystemVersion v = {};
        if (MCP_GetSystemVersion(mcp, &v) == 0)
            tx_add(t, ",\"sistema\":\"%u.%u.%u%c\"", (unsigned) v.major, (unsigned) v.minor, (unsigned) v.patch,
                   v.region ? v.region : '?');
        alignas(0x40) static MCPSysProdSettings prod;
        if (MCP_GetSysProdSettings(mcp, &prod) == 0) {
            char modelo[17] = "";
            memcpy(modelo, prod.model_number, 16);
            tx_add(t, ",\"modelo\":");
            tx_json(t, modelo);
        }
        MCP_Close(mcp);
    }

    void sd_json(Texto *t) {
        uint64_t libre, total;
        if (espacio_sd(&libre, &total)) tx_add(t, ",\"sd_libre\":%llu,\"sd_total\":%llu", libre, total);
    }

    void responder_lista(Peticion *p, void (*rellenar)(Texto *)) {
        Texto t = {};
        rellenar(&t);
        http_json(p->sock, &t);
    }
} // namespace

void ordenes_programar(Despues d) { pendiente = d; }

Despues ordenes_pendiente() {
    Despues d = pendiente;
    pendiente = DESPUES_NADA;
    return d;
}

void orden_estado(Peticion *p) {
    http_texto(p->sock, 200, "{\"version\":\"%s\",\"titulo\":\"%016llx\",\"encendida_s\":%llu}",
               TITIRITERO_VERSION, OSGetTitleID(), segundos_encendida());
}

void orden_info(Peticion *p) {
    uint64_t id = OSGetTitleID();
    char nombre[256] = "";
    if (!nombre_titulo(id, nombre, sizeof nombre)) nombre[0] = 0;
    Texto t = {};
    tx_add(&t, "{\"version\":\"%s\",\"titulo\":\"%016llx\",\"titulo_nombre\":", TITIRITERO_VERSION, id);
    tx_json(&t, nombre);
    tx_add(&t, ",\"encendida_s\":%llu,\"bateria\":%d", segundos_encendida(), mando_bateria());
    sistema_json(&t);
    sd_json(&t);
    tx_add(&t, ",\"en_pausa\":%s,\"ftp_abierto\":%s", ajustes_servidor_activo() ? "false" : "true",
           sistema_ftp_abierto() ? "true" : "false");
    tx_add(&t, ",\"permisos\":{\"ver\":%d,\"mandos\":%d,\"abrir\":%d,\"energia\":%d,\"avisos\":%d,"
               "\"leer\":%d,\"escribir\":%d}",
           ajustes_permite(PERMISO_VER), ajustes_permite(PERMISO_MANDOS), ajustes_permite(PERMISO_ABRIR),
           ajustes_permite(PERMISO_ENERGIA), ajustes_permite(PERMISO_AVISOS), ajustes_permite(PERMISO_LEER),
           ajustes_permite(PERMISO_ESCRIBIR));
    tx_add(&t, "}");
    http_json(p->sock, &t);
}

void orden_registro(Peticion *p) {
    static char copia[16 * 1024];
    uint32_t n = registro_copiar(copia, sizeof copia);
    http_responder(p->sock, 200, "text/plain; charset=utf-8", copia, n);
}

void orden_aviso(Peticion *p) {
    char texto[200];
    if (!http_param(p, "texto", texto, sizeof texto) || !texto[0]) return http_texto(p->sock, 400, "falta texto");
    char tipo[8] = "info";
    http_param(p, "tipo", tipo, sizeof tipo);
    if (!strcmp(tipo, "error")) NotificationModule_AddErrorNotification(texto);
    else NotificationModule_AddInfoNotification(texto);
    http_texto(p->sock, 200, "mostrado");
}

void orden_titulos(Peticion *p) {
    Texto t = {};
    titulos_json(&t, http_param_num(p, "fresco", 0, 0, 1) == 1);
    http_json(p->sock, &t);
}

void orden_apps(Peticion *p) { responder_lista(p, apps_json); }

void orden_abrir(Peticion *p) {
    char ruta[400];
    if (!http_param(p, "ruta", ruta, sizeof ruta) || strstr(ruta, "..") || strchr(ruta, ':') || strchr(ruta, '\\'))
        return http_texto(p->sock, 400, "falta ruta (relativa a la SD, ej. wiiu/apps/x/x.wuhb)");
    const char *motivo = "";
    if (!sistema_abrir(ruta, &motivo)) return http_texto(p->sock, 500, "%s", motivo);
    aviso("abriendo una app desde el servidor");
    http_texto(p->sock, 200, "abriendo");
}

void orden_titulo(Peticion *p) {
    char v[24];
    if (!http_param(p, "id", v, sizeof v)) return http_texto(p->sock, 400, "falta id");
    const char *motivo = "";
    if (!sistema_abrir_titulo(strtoull(v, nullptr, 16), &motivo)) return http_texto(p->sock, 500, "%s", motivo);
    http_texto(p->sock, 200, "abriendo");
}

void orden_menu(Peticion *p) {
    http_texto(p->sock, 200, "volviendo al menu");
    ordenes_programar(DESPUES_MENU);
}

void orden_reiniciar(Peticion *p) {
    http_texto(p->sock, 200, "reiniciando");
    ordenes_programar(DESPUES_REINICIAR);
}

/* Desde aquí no se puede volver a encender: lo pide la orden del servidor con confirmación. */
void orden_apagar(Peticion *p) {
    if (http_param_num(p, "seguro", 0, 0, 1) != 1) return http_texto(p->sock, 400, "falta seguro=1");
    http_texto(p->sock, 200, "apagando");
    ordenes_programar(DESPUES_APAGAR);
}

/* Guarda la clave nueva en la SD (con fsync) y se aplica al responder: la respuesta va con la vieja. */
void orden_clave(Peticion *p) {
    static char nueva[64];
    if (!firma_descifrar_clave(p, nueva, sizeof nueva)) return http_texto(p->sock, 400, "clave nueva no valida");
    char tmp[] = RUTA_CLAVE ".nueva";
    FILE *f = fopen(tmp, "w");
    bool ok = f && fprintf(f, "%s\n", nueva) > 0 && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (f) ok = fclose(f) == 0 && ok;
    if (!ok || (remove(RUTA_CLAVE), rename(tmp, RUTA_CLAVE)) != 0) {
        remove(tmp);
        return http_texto(p->sock, 500, "no se pudo guardar la clave nueva; sigue valiendo la anterior");
    }
    registro("clave cambiada desde el servidor");
    http_texto(p->sock, 200, "clave cambiada");
    ordenes_programar(DESPUES_CLAVE);
}

/* El propio móvil se quita de la lista (botón «Olvidar este móvil» del panel). */
void orden_olvidar(Peticion *p) {
    if (p->disp == ID_SERVIDOR) return http_texto(p->sock, 400, "la clave del servidor se borra en la SD");
    http_texto(p->sock, 200, "olvidado");
    dispositivos_quitar(p->disp);
}

/* «Emparejar otro móvil»: un aparato ya emparejado pide que la consola enseñe un QR nuevo.
 * El código no se devuelve: solo se ve en la pantalla de la consola. */
void orden_invitar(Peticion *p) {
    if (http_param_num(p, "ocultar", 0, 0, 1) == 1) {
        superposicion_ocultar();
        return http_texto(p->sock, 200, "qr oculto");
    }
    ajustes_mostrar_qr(true);
    http_texto(p->sock, 200, "qr en la pantalla de la consola");
}
