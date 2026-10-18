#pragma once
/* Números aleatorios seguros. La Wii U no da una fuente de entropía a los plugins, así que se junta:
 * el ruido de los sensores del GamePad (acelerómetro, giroscopio, magnetómetro) y la hora fina del
 * procesador en cada lectura del mando, más una semilla guardada del arranque anterior. Todo pasa por
 * SHA-256 y alimenta un CTR_DRBG de mbedTLS. */
#include <cstddef>
#include <cstdint>

void entropia_iniciar();
/* La llama el gancho de VPADRead con cada lectura: mezcla los bytes en la reserva. */
void entropia_sumar(const void *datos, uint32_t largo);
/* Rellena con bytes aleatorios. false si aún no hay entropía suficiente (no debería pasar tras unos segundos). */
bool entropia_bytes(uint8_t *destino, uint32_t largo);
/* Guarda una semilla nueva para el próximo arranque (almacenamiento de WUPS). */
void entropia_guardar_semilla();
/* Para mbedTLS: f_rng de las funciones que piden aleatoriedad. */
int entropia_rng(void *, unsigned char *destino, size_t largo);
