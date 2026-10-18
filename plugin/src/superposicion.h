#pragma once
/* Pantalla de emparejamiento con el QR, a pantalla completa, con el procesador y sin órdenes a la GPU
 * (ver superposicion.cpp: el intento con GX2CopySurfaceEx colgó el menú de la Wii U). */
#include <cstdint>
#include <gx2/surface.h>

void superposicion_iniciar();
/* texto_url va dentro del QR; debajo se escriben `codigo` (en grande) y `direccion` (solo cifras, '.' y ':'). */
bool superposicion_mostrar(const char *texto_url, const char *codigo, const char *direccion);
void superposicion_ocultar();
bool superposicion_visible();
/* La llama el gancho de GX2SwapScanBuffers, ya con la GPU parada (GX2DrawDone): pantalla 0 = tele, 1 = GamePad. */
void superposicion_en_escaneo(const GX2ColorBuffer *scan, int pantalla);
/* Copia lineal RGBA (pitch = ancho) de lo que se ve, para las capturas; liberar con MEMFreeToMappedMemory. */
void *superposicion_foto(int pantalla, uint32_t *ancho, uint32_t *alto);
