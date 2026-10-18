#include "sistema.h"
#include "comun.h"

#include <coreinit/launch.h>
#include <coreinit/title.h>
#include <nn/act.h>
#include <rpxloader/rpxloader.h>
#include <sysapp/launch.h>
#include <sysapp/title.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <sys/select.h>
#include <coreinit/time.h>
#include <nn/ac/ac_c.h>

namespace {
    bool rpx_listo;

    /* Solo se anota: el mismo chequeo que hace wiiload decía "sin usuario" con el Mii cargado. */
    void anotar_usuario() {
        nn::act::Initialize();
        auto slot = nn::act::GetSlotNo();
        registro("usuario: ranura %u ocupada %d, %u cuentas", (unsigned) slot,
                 (int) nn::act::IsSlotOccupied(slot), (unsigned) nn::act::GetNumOfAccounts());
        nn::act::Finalize();
    }
} // namespace

void sistema_iniciar() {
    rpx_listo = RPXLoader_InitLibrary() == RPX_LOADER_RESULT_SUCCESS;
    if (!rpx_listo) registro("RPXLoader no disponible: no se podran abrir homebrew");
}

void sistema_cerrar() {
    if (rpx_listo) RPXLoader_DeInitLibrary();
    rpx_listo = false;
}

bool sistema_abrir(const char *ruta, const char **motivo) {
    if (!rpx_listo) return *motivo = "RPXLoader no disponible", false;
    anotar_usuario();
    RPXLoaderStatus r = RPXLoader_LaunchHomebrew(ruta);
    if (r != RPX_LOADER_RESULT_SUCCESS) return *motivo = RPXLoader_GetStatusStr(r), false;
    registro("abriendo %s", ruta);
    return true;
}

bool sistema_abrir_titulo(uint64_t id, const char **motivo) {
    if (!SYSCheckTitleExists(id)) return *motivo = "ese titulo no esta instalado", false;
    registro("abriendo titulo %016llx", id);
    SYSLaunchTitle(id);
    return true;
}

void sistema_menu() {
    registro("volviendo al menu");
    SYSLaunchMenu();
}

/* Igual que la app WiiUReboot: relanzado completo al volver al menú = reinicio. */
void sistema_reiniciar() {
    registro("reiniciando la consola");
    OSForceFullRelaunch();
    SYSLaunchMenu();
}

void sistema_apagar() {
    registro("apagando la consola");
    OSShutdown();
}

namespace {
    /* Conexión sin bloquear, con límite de tiempo: en la Wii U conectar a 127.0.0.1 se queda colgado
     * más de un minuto, y esto también se llama desde el menú de ajustes (hilo del juego). */
    bool puerto_abierto(uint32_t ip_red, uint16_t puerto, int ms) {
        int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s < 0) return false;
        int si = 1;
        setsockopt(s, SOL_SOCKET, SO_NONBLOCK, &si, sizeof si);
        sockaddr_in dir = {};
        dir.sin_family = AF_INET;
        dir.sin_port = htons(puerto);
        dir.sin_addr.s_addr = ip_red;
        bool abierto = connect(s, (sockaddr *) &dir, sizeof dir) == 0;
        if (!abierto) {
            fd_set escritura;
            FD_ZERO(&escritura);
            FD_SET(s, &escritura);
            timeval t = {0, ms * 1000};
            int error = 1;
            socklen_t largo = sizeof error;
            abierto = select(s + 1, nullptr, &escritura, nullptr, &t) > 0 &&
                      getsockopt(s, SOL_SOCKET, SO_ERROR, &error, &largo) == 0 && error == 0;
        }
        close(s);
        return abierto;
    }
} // namespace

bool sistema_ftp_abierto() {
    static OSTime ultima;
    static bool cache;
    if (ultima && OSTicksToSeconds(OSGetTime() - ultima) < 30) return cache;
    uint32_t ip = 0;
    ACInitialize();
    bool hay_ip = NNResult_IsSuccess(ACGetAssignedAddress(&ip)) && ip;
    ACFinalize();
    cache = hay_ip && puerto_abierto(htonl(ip), 21, 300);
    ultima = OSGetTime();
    return cache;
}

std::string sistema_ip() {
    uint32_t ip = 0;
    ACInitialize();
    bool ok = NNResult_IsSuccess(ACGetAssignedAddress(&ip)) && ip;
    ACFinalize();
    if (!ok) return "sin red";
    char t[20];
    snprintf(t, sizeof t, "%u.%u.%u.%u", (unsigned) (ip >> 24) & 255, (unsigned) (ip >> 16) & 255, (unsigned) (ip >> 8) & 255,
             (unsigned) ip & 255);
    return t;
}
