#pragma once
#include <cstdint>

/* Pulsaciones falsas del GamePad: se mezclan con las reales dentro de VPADRead. */

/* Prepara el estado; se llama al cargar el plugin, antes de cualquier VPADRead. */
void mando_iniciar();

/* Convierte "A,B,ARRIBA" en la máscara de botones de VPAD. Devuelve false si algún nombre no existe. */
bool mando_botones(const char *nombres, uint32_t *mascara);

/* Mantiene la máscara pulsada durante ms y la suelta. Bloquea hasta terminar. */
void mando_pulsar(uint32_t mascara, uint32_t ms);

/* Toca la pantalla del GamePad en (x, y), coordenadas de 854x480, durante ms. */
bool mando_tocar(int x, int y, uint32_t ms);

/* Inclina una palanca (0 izquierda, 1 derecha) con x, y en [-1, 1] durante ms. */
void mando_palanca(int cual, float x, float y, uint32_t ms);

/* Nivel de batería del GamePad (0 vacía .. 6 cargando/llena) o -1 si no se sabe. */
int mando_bateria();

/* Hace vibrar el GamePad durante ms (útil para encontrarlo). */
void mando_vibrar(uint32_t ms);
