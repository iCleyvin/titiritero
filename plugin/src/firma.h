#pragma once
/* Órdenes firmadas: ninguna clave viaja por la red.
 *
 * Cada aparato emparejado (dispositivos.h) tiene su clave. Para dar órdenes:
 * 1. GET /reto?disp=<id> → {sesion, reto}. El reto es un HMAC con la clave del aparato y datos de este
 *    arranque más azar: nadie sin la clave puede predecirlo, y cambia en cada arranque.
 * 2. Cada orden lleva X-Sesion, X-Contador (estrictamente creciente por sesión), X-Cuerpo (SHA-256 del
 *    cuerpo, en hex) y X-Firma = HMAC-SHA256(clave, reto \n contador \n método \n ruta \n consulta \n X-Cuerpo).
 * Una orden capturada no se puede repetir (contador) ni modificar (firma), ni el archivo subido (X-Cuerpo). */
#include "http.h"
#include <cstdint>

struct Sha256;   /* contexto opaco para ir calculando el hash de un cuerpo grande */

/* Borra todas las sesiones (al arrancar o al cambiar la clave del servidor). */
void firma_reiniciar();
/* Crea una sesión para el aparato y escribe {"sesion":N,"reto":"..."}. false si el aparato no existe. */
bool firma_reto(uint32_t disp, char *json, uint32_t cap);
/* Comprueba la firma; en *disp, el aparato que la firmó; en *motivo, por qué no vale. */
bool firma_valida(const Peticion *p, uint32_t *disp, const char **motivo);

Sha256 *sha256_nuevo();
void sha256_sumar(Sha256 *s, const void *datos, uint32_t largo);
/* Termina, escribe el hash en hex (65 bytes con el 0) y libera el contexto. */
void sha256_fin(Sha256 *s, char *hex);

/* Cambio de la clave del servidor sin que la nueva viaje en claro: el parámetro c (firmado, va en la consulta)
 * lleva la clave nueva (48 hex) cifrada con un flujo HMAC(clave vieja, "rotar" \n reto \n contador \n bloque).
 * Solo vale para el aparato 0 (servidor). Devuelve false si no descifra a 48 cifras hex. */
bool firma_descifrar_clave(const Peticion *p, char *nueva, uint32_t cap);
