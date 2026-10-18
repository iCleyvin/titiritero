#include "ajustes.h"
#include "comun.h"
#include "dispositivos.h"
#include "idioma.h"
#include "sistema.h"
#include "superposicion.h"

#include <coreinit/time.h>
#include <nn/ac/ac_c.h>
#include <notifications/notifications.h>
#include <wups.h>
#include <wups/button_combo/api.h>
#include <wups/config/WUPSConfigCategory.h>
#include <wups/config/WUPSConfigItemBoolean.h>
#include <wups/config/WUPSConfigItemButtonCombo.h>
#include <wups/config/WUPSConfigItemMultipleValues.h>
#include <wups/config/WUPSConfigItemStub.h>
#include <wups/storage.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <forward_list>
#include <string>

namespace {
    struct Interruptor {
        const char *id;
        const char *es, *en;   /* texto en el menú */
        bool defecto;
        bool valor;
    };

    /* Orden = orden en el menú. Todo encendido de serie: nada funciona hasta emparejar un móvil. */
    Interruptor servidor = {"servidor", "Control remoto", "Remote control", true, true};
    Interruptor avisar = {"avisar", "Avisar cuando alguien empiece a manejarla", "Notify when someone starts controlling it",
                          true, true};
    Interruptor permisos[] = {
        {"ver", "Ver la pantalla", "See the screen", true, true},
        {"mandos", "Pulsar botones y tocar la pantalla", "Press buttons and touch the screen", true, true},
        {"abrir", "Abrir juegos y apps", "Open games and apps", true, true},
        {"energia", "Volver al menú, reiniciar y apagar", "Go to the menu, restart and power off", true, true},
        {"avisos", "Mensajes en pantalla y vibración", "On-screen messages and rumble", true, true},
    };
    enum { P_VER, P_MANDOS, P_ABRIR, P_ENERGIA, P_AVISOS };

    uint32_t archivos = ARCHIVOS_LEER;   /* subir archivos deja instalar plugins: lo activa el dueño */
    uint32_t fps = 15;
    constexpr uint32_t COMBO_DEFECTO = WUPS_BUTTON_COMBO_BUTTON_L | WUPS_BUTTON_COMBO_BUTTON_R | WUPS_BUTTON_COMBO_BUTTON_MINUS;
    uint32_t combo = COMBO_DEFECTO;

    WUPSButtonCombo_ComboHandle combo_handle(nullptr);
    std::forward_list<WUPSButtonComboAPI::ButtonCombo> combos;

    uint32_t ultimo_disp;
    OSTime ultima_hora;

    /* Lo elegido en el menú se aplica al cerrarlo. */
    char codigo[8];
    bool quiere_qr;
    uint32_t quitar[MAX_DISPOSITIVOS];
    int n_quitar;

    /* ---------- almacenamiento ---------- */

    void cargar(Interruptor &i) {
        if (WUPSStorageAPI::GetOrStoreDefault(i.id, i.valor, i.defecto) != WUPS_STORAGE_ERROR_SUCCESS) i.valor = i.defecto;
    }

    Interruptor *buscar(const char *id) {
        if (!strcmp(id, servidor.id)) return &servidor;
        if (!strcmp(id, avisar.id)) return &avisar;
        for (auto &p : permisos)
            if (!strcmp(id, p.id)) return &p;
        return nullptr;
    }

    const char *texto(const Interruptor &i) { return tr(i.es, i.en); }

    /* ---------- control remoto encendido/en pausa ---------- */

    /* No se para el hilo: esto corre en el hilo del juego (menú o combinación) y esperar a que
     * acabe una orden en curso lo congelaría. En pausa, el servidor rechaza las órdenes. */
    void poner_servidor(bool activo) {
        servidor.valor = activo;
        WUPSStorageAPI::Store(servidor.id, activo);
        registro("control remoto %s", activo ? "activado" : "en pausa");
    }

    void alternar_por_combo(WUPSButtonCombo_ControllerTypes, WUPSButtonCombo_ComboHandle, void *) {
        poner_servidor(!servidor.valor);
        WUPSStorageAPI::SaveStorage();
        if (servidor.valor) NotificationModule_AddInfoNotification(tr("Titiritero: control remoto activado", "Titiritero: remote control on"));
        else NotificationModule_AddErrorNotification(tr("Titiritero: control remoto en pausa (vuelve con la misma combinación)",
                                                        "Titiritero: remote control paused (same buttons to resume)"));
    }

    void registrar_combo() {
        WUPSButtonCombo_ComboStatus estado;
        WUPSButtonCombo_Error error = WUPS_BUTTON_COMBO_ERROR_UNKNOWN_ERROR;
        auto c = WUPSButtonComboAPI::CreateComboPressDown("Titiritero", (WUPSButtonCombo_Buttons) combo, alternar_por_combo,
                                                          nullptr, estado, error);
        if (!c) return registro("no se pudo registrar la combinacion: %s", WUPSButtonComboAPI::GetStatusStr(error));
        combo_handle = c->getHandle();
        combos.emplace_front(std::move(*c));
        if (estado == WUPS_BUTTON_COMBO_COMBO_STATUS_CONFLICT) registro("la combinacion choca con la de otro plugin");
    }

    /* ---------- callbacks del menú ---------- */

    void al_cambiar_interruptor(ConfigItemBoolean *item, bool v) {
        if (!item || !item->identifier) return;
        if (!strcmp(item->identifier, servidor.id)) return poner_servidor(v);
        if (!strcmp(item->identifier, "qr")) return (void) (quiere_qr = v);
        if (!strncmp(item->identifier, "disp_", 5)) {
            uint32_t id = (uint32_t) strtoul(item->identifier + 5, nullptr, 10);
            if (!v && n_quitar < MAX_DISPOSITIVOS) quitar[n_quitar++] = id;
            if (v)
                for (int i = 0; i < n_quitar; ++i)
                    if (quitar[i] == id) quitar[i] = 0;
            return;
        }
        Interruptor *i = buscar(item->identifier);
        if (!i) return;
        i->valor = v;
        WUPSStorageAPI::Store(i->id, v);
    }

    void al_cambiar_lista(ConfigItemMultipleValues *item, uint32_t v) {
        if (!item || !item->identifier) return;
        if (!strcmp(item->identifier, "archivos")) archivos = v;
        if (!strcmp(item->identifier, "fps")) fps = v;
        WUPSStorageAPI::Store(item->identifier, v);
    }

    void al_cambiar_combo(ConfigItemButtonCombo *item, uint32_t v) {
        combo = v;
        if (item && item->identifier) WUPSStorageAPI::Store(item->identifier, v);
    }

    /* ---------- textos ---------- */

    std::string actividad() {
        if (!ultima_hora) return tr("Nadie la ha manejado desde que se encendió", "Nobody has controlled it since it was turned on");
        auto s = OSTicksToSeconds(OSGetSystemTime() - ultima_hora);
        char t[120];
        if (s < 60) snprintf(t, sizeof t, tr("Última orden hace %llu s, de %s", "Last order %llu s ago, from %s"),
                             (unsigned long long) s, dispositivos_nombre(ultimo_disp));
        else snprintf(t, sizeof t, tr("Última orden hace %llu min, de %s", "Last order %llu min ago, from %s"),
                      (unsigned long long) (s / 60), dispositivos_nombre(ultimo_disp));
        return t;
    }

    void anadir_estado(WUPSConfigCategory &raiz) {
        char t[120];
        std::string ip = sistema_ip();
        snprintf(t, sizeof t, tr("Panel en el móvil: http://%s:%d", "Phone panel: http://%s:%d"), ip.c_str(), TITIRITERO_PUERTO);
        raiz.add(WUPSConfigItemStub::Create(t));
        raiz.add(WUPSConfigItemStub::Create(actividad()));
        if (sistema_ftp_abierto())
            raiz.add(WUPSConfigItemStub::Create(tr("AVISO: el FTP (ftpiiu) está encendido: cualquiera de tu red puede entrar en la SD",
                                                   "WARNING: FTP (ftpiiu) is on: anyone on your network can open your SD card")));
    }

    void anadir_emparejar(WUPSConfigCategory &raiz) {
        auto cat = WUPSConfigCategory::Create(tr("Emparejar un móvil", "Pair a phone"));
        quiere_qr = false;
        cat.add(WUPSConfigItemBoolean::CreateEx("qr", tr("Mostrar el QR al cerrar este menú", "Show the QR code when this menu closes"),
                                                false, false, al_cambiar_interruptor, tr("Sí", "Yes"), "No"));
        char t[120];
        snprintf(t, sizeof t, tr("O escribe el código %.3s %.3s en el panel (vale 5 minutos)",
                                 "Or type the code %.3s %.3s in the panel (valid for 5 minutes)"), codigo, codigo + 3);
        cat.add(WUPSConfigItemStub::Create(t));
        raiz.add(std::move(cat));
    }

    void anadir_aparatos(WUPSConfigCategory &raiz) {
        auto cat = WUPSConfigCategory::Create(tr("Móviles emparejados", "Paired phones"));
        Dispositivo lista[MAX_DISPOSITIVOS];
        int n = dispositivos_lista(lista, MAX_DISPOSITIVOS);
        n_quitar = 0;
        if (!n) cat.add(WUPSConfigItemStub::Create(tr("Ninguno todavía", "None yet")));
        for (int i = 0; i < n; ++i) {
            char id[16];
            snprintf(id, sizeof id, "disp_%u", (unsigned) lista[i].id);
            cat.add(WUPSConfigItemBoolean::CreateEx(std::string(id), lista[i].nombre, true, true, al_cambiar_interruptor,
                                                    tr("Conservar", "Keep"), tr("Quitar", "Remove")));
        }
        raiz.add(std::move(cat));
    }

    void anadir_control(WUPSConfigCategory &raiz) {
        auto cat = WUPSConfigCategory::Create(tr("Control remoto", "Remote control"));
        cat.add(WUPSConfigItemBoolean::CreateEx(servidor.id, texto(servidor), servidor.defecto, servidor.valor,
                                                al_cambiar_interruptor, tr("Activado", "On"), tr("En pausa", "Paused")));
        cat.add(WUPSConfigItemButtonCombo::Create("combo", tr("Pausar o reanudar al momento", "Pause or resume instantly"),
                                                  (WUPSButtonCombo_Buttons) COMBO_DEFECTO, combo_handle, al_cambiar_combo));
        cat.add(WUPSConfigItemBoolean::CreateEx(avisar.id, texto(avisar), avisar.defecto, avisar.valor, al_cambiar_interruptor,
                                                tr("Sí", "Yes"), "No"));
        static constexpr WUPSConfigItemMultipleValues::ValuePair OPC_FPS[] = {{5, "5"}, {10, "10"}, {15, "15"}, {30, "30"}};
        cat.add(WUPSConfigItemMultipleValues::CreateFromValue("fps", tr("Imágenes por segundo en el panel (menos = juegos más fluidos)",
                                                                        "Frames per second in the panel (fewer = smoother games)"),
                                                              15, fps, OPC_FPS, al_cambiar_lista));
        raiz.add(std::move(cat));
    }

    void anadir_permisos(WUPSConfigCategory &raiz) {
        auto cat = WUPSConfigCategory::Create(tr("Qué se puede hacer desde el móvil", "What phones can do"));
        for (auto &p : permisos)
            cat.add(WUPSConfigItemBoolean::CreateEx(p.id, texto(p), p.defecto, p.valor, al_cambiar_interruptor, tr("Sí", "Yes"), "No"));
        static const WUPSConfigItemMultipleValues::ValuePair OPC_ES[] = {
                {ARCHIVOS_NO, "No"}, {ARCHIVOS_LEER, "Solo ver y descargar"}, {ARCHIVOS_TODO, "Ver, subir y borrar"}};
        static const WUPSConfigItemMultipleValues::ValuePair OPC_EN[] = {
                {ARCHIVOS_NO, "No"}, {ARCHIVOS_LEER, "View and download only"}, {ARCHIVOS_TODO, "View, upload and delete"}};
        cat.add(WUPSConfigItemMultipleValues::CreateFromValue("archivos", tr("Archivos de la tarjeta SD", "SD card files"),
                                                              ARCHIVOS_LEER, archivos, idioma_es() ? OPC_ES : OPC_EN,
                                                              al_cambiar_lista));
        raiz.add(std::move(cat));
    }

    /* Últimas líneas del registro, la más reciente arriba. */
    void anadir_actividad(WUPSConfigCategory &raiz) {
        auto cat = WUPSConfigCategory::Create(tr("Actividad reciente", "Recent activity"));
        static char copia[4096];
        uint32_t n = registro_copiar(copia, sizeof copia - 1);
        copia[n] = 0;
        const char *lineas[12];
        int total = 0;
        for (char *l = strtok(copia, "\n"); l; l = strtok(nullptr, "\n")) {
            if (total == 12) memmove(lineas, lineas + 1, sizeof lineas - sizeof *lineas), --total;
            lineas[total++] = l;
        }
        if (!total) cat.add(WUPSConfigItemStub::Create(tr("Sin actividad todavía", "No activity yet")));
        for (int i = total - 1; i >= 0; --i) cat.add(WUPSConfigItemStub::Create(lineas[i]));
        raiz.add(std::move(cat));
    }

    WUPSConfigAPICallbackStatus al_abrir(WUPSConfigCategoryHandle h) {
        emparejar_nuevo_codigo(codigo, sizeof codigo);   /* abrir el menú = estar delante de la consola */
        try {
            WUPSConfigCategory raiz(h);
            anadir_estado(raiz);
            anadir_emparejar(raiz);
            anadir_aparatos(raiz);
            anadir_control(raiz);
            anadir_permisos(raiz);
            anadir_actividad(raiz);
        } catch (std::exception &e) {
            registro("menu de ajustes: %s", e.what());
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }
        return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
    }

    void al_cerrar() {
        for (int i = 0; i < n_quitar; ++i)
            if (quitar[i]) dispositivos_quitar(quitar[i]);
        n_quitar = 0;
        WUPSStorageAPI::SaveStorage();
        if (quiere_qr) ajustes_mostrar_qr(false);
    }
} // namespace

void ajustes_iniciar() {
    cargar(servidor);
    cargar(avisar);
    for (auto &p : permisos) cargar(p);
    WUPSStorageAPI::GetOrStoreDefault<uint32_t>("archivos", archivos, ARCHIVOS_LEER);
    WUPSStorageAPI::GetOrStoreDefault<uint32_t>("fps", fps, 15);
    WUPSStorageAPI::GetOrStoreDefault<uint32_t>("combo", combo, COMBO_DEFECTO);
    if (!combo) combo = COMBO_DEFECTO;
    WUPSStorageAPI::DeleteItem("origen");   /* ajuste de la 0.3 que ya no existe */
    WUPSStorageAPI::SaveStorage();
    registrar_combo();
    WUPSConfigAPIOptionsV1 opciones = {.name = "Titiritero"};
    if (WUPSConfigAPI_Init(opciones, al_abrir, al_cerrar) != WUPSCONFIG_API_RESULT_SUCCESS)
        registro("no se pudo crear el menu de ajustes");
}

/* Pinta el QR de emparejamiento en la pantalla de la consola. Con codigo_nuevo crea antes un código
 * (cuando lo pide un móvil ya emparejado desde el panel); si no, usa el del menú. */
void ajustes_mostrar_qr(bool codigo_nuevo) {
    if (codigo_nuevo) emparejar_nuevo_codigo(codigo, sizeof codigo);
    std::string ip = sistema_ip();
    char url[96], dir[40];
    snprintf(url, sizeof url, "http://%s:%d/#%s", ip.c_str(), TITIRITERO_PUERTO, codigo);
    snprintf(dir, sizeof dir, "%s:%d", ip.c_str(), TITIRITERO_PUERTO);
    if (!superposicion_mostrar(url, codigo, dir)) registro("no se pudo dibujar el QR");
}

void ajustes_cerrar() { combos.clear(); }

/* Una vez por encendido: que se sepa que está y cómo conectar el primer móvil. */
void ajustes_bienvenida() {
    static bool hecho;
    if (hecho || !avisar.valor) return;
    hecho = true;
    Dispositivo hay[1];
    if (dispositivos_lista(hay, 1))
        NotificationModule_AddInfoNotification(tr("Titiritero listo", "Titiritero ready"));
    else
        NotificationModule_AddInfoNotification(tr("Titiritero listo: para conectar el móvil, pulsa L + abajo + menos",
                                                  "Titiritero ready: to connect your phone, press L + down + minus"));
}

bool ajustes_servidor_activo() { return servidor.valor; }

uint32_t ajustes_fps() { return fps ? fps : 15; }

bool ajustes_permite(Permiso p) {
    switch (p) {
        case PERMISO_SIEMPRE: return true;
        case PERMISO_VER: return permisos[P_VER].valor;
        case PERMISO_MANDOS: return permisos[P_MANDOS].valor;
        case PERMISO_ABRIR: return permisos[P_ABRIR].valor;
        case PERMISO_ENERGIA: return permisos[P_ENERGIA].valor;
        case PERMISO_AVISOS: return permisos[P_AVISOS].valor;
        case PERMISO_LEER: return archivos >= ARCHIVOS_LEER;
        case PERMISO_ESCRIBIR: return archivos == ARCHIVOS_TODO;
        case PERMISO_ADMIN: return true;
    }
    return false;
}

void ajustes_actividad(uint32_t disp, const char *) {
    /* Una sesión por aparato: varios a la vez no deben llenar el registro ni la pantalla de avisos. */
    struct Visto { uint32_t disp; OSTime hora; };
    static Visto vistos[12];
    OSTime ahora = OSGetSystemTime();
    ultimo_disp = disp;
    ultima_hora = ahora;
    Visto *v = nullptr, *mas_viejo = &vistos[0];
    for (Visto &x : vistos) {
        if (x.hora && x.disp == disp) v = &x;
        if (x.hora < mas_viejo->hora) mas_viejo = &x;
    }
    bool nueva_sesion = !v || OSTicksToSeconds(ahora - v->hora) > 120;
    if (!v) v = mas_viejo;
    *v = {disp, ahora};
    if (!nueva_sesion) return;
    registro("sesion de control: %s", dispositivos_nombre(disp));
    if (!avisar.valor) return;
    char t[96];
    snprintf(t, sizeof t, tr("Titiritero: %s está manejando la consola", "Titiritero: %s is controlling the console"),
             dispositivos_nombre(disp));
    NotificationModule_AddInfoNotification(t);
}
