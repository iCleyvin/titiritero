/*
 * Titiritero — maneja la Wii U desde el navegador del móvil: la consola sirve el panel,
 * se empareja con un QR y cada orden va firmada.
 */
#include "ajustes.h"
#include "captura.h"
#include "superposicion.h"
#include "comun.h"
#include "dispositivos.h"
#include "entropia.h"
#include "idioma.h"
#include "mando.h"
#include "servidor.h"
#include "sistema.h"

#include <notifications/notifications.h>
#include <wups.h>

WUPS_PLUGIN_NAME("Titiritero");
WUPS_PLUGIN_DESCRIPTION("Maneja la consola desde el navegador del móvil: pantalla en vivo, mandos, juegos y archivos");
WUPS_PLUGIN_VERSION(TITIRITERO_VERSION);
WUPS_PLUGIN_AUTHOR("Cleyvin");
WUPS_PLUGIN_LICENSE("GPL3");

WUPS_USE_WUT_DEVOPTAB();   /* para leer la clave de la SD */
WUPS_USE_STORAGE("titiritero");   /* ajustes del menú de plugins */

INITIALIZE_PLUGIN() {
    NotificationModule_InitLibrary();
    idioma_iniciar();
    entropia_iniciar();
    dispositivos_iniciar();
    mando_iniciar();
    captura_iniciar();
    superposicion_iniciar();
    sistema_iniciar();
    ajustes_iniciar();
    registro("Titiritero %s cargado", TITIRITERO_VERSION);
}

DEINITIALIZE_PLUGIN() {
    ajustes_cerrar();
    sistema_cerrar();
    NotificationModule_DeInitLibrary();
}

ON_APPLICATION_START() {
    servidor_arrancar();   /* en pausa responde, pero rechaza las órdenes */
    ajustes_bienvenida();
}

ON_APPLICATION_ENDS() {
    servidor_parar();
    entropia_guardar_semilla();   /* la próxima vez arranca con azar ya acumulado */
}
