#pragma once
/*
 * Titiritero — plugin de Aroma que deja manejar la Wii U desde el servidor.
 * Licencia GPL-3.0 (la captura de pantalla sigue el método de ScreenshotWUPS).
 */
#include <cstdint>

#define TITIRITERO_VERSION "0.6.0"
#define TITIRITERO_PUERTO  8590
#define CARPETA_SD         "fs:/vol/external01/wiiu/titiritero"
#define RUTA_CLAVE         CARPETA_SD "/clave.txt"

/* Registro interno: un búfer circular que el servidor lee con GET /registro. */
void registro(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Copia el registro en orden cronológico; devuelve los bytes escritos. */
uint32_t registro_copiar(char *destino, uint32_t capacidad);

/* Muestra un aviso en la pantalla de la consola (módulo de notificaciones de Aroma). */
void aviso(const char *texto);
