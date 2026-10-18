#pragma once
#include <cstdint>

/* Captura de la tele (0) o el GamePad (1) como JPEG.
 * Se pide desde el hilo del servidor; la copia la hace el hilo de dibujo de la app
 * dentro de GX2CopyColorBufferToScanBuffer, en el siguiente fotograma que muestre. */

void captura_iniciar();

/* Devuelve un JPEG reservado con tjAlloc (liberar con captura_liberar) o nullptr.
 * Con mitad, la imagen sale a la mitad de ancho y alto (más rápida para la vista en vivo).
 * En *motivo queda una explicación corta si falla. */
uint8_t *captura_jpeg(int pantalla, int calidad, bool mitad, uint32_t *tamano, const char **motivo);

void captura_liberar(uint8_t *jpeg);

/* Despierta a una captura que espera un fotograma (al cerrar la app ya no llegará ninguno). */
void captura_cancelar();

/* JSON con cuántas veces se ha llamado a cada gancho (GET /diagnostico). */
void captura_diagnostico(char *json, uint32_t cap);
