#pragma once
/* Móviles (y otros aparatos) emparejados con la consola, y el emparejamiento.
 *
 * Emparejar (X25519 + código de 6 cifras que solo se ve en la pantalla de la consola):
 *   1. El menú de Titiritero genera un código de un solo uso (vale 5 minutos, 5 intentos).
 *   2. POST /emparejar?pub=<clave pública X25519 del móvil>&nombre=<nombre>
 *      → {"pub": <clave pública de la consola>}. Los dos calculan el mismo secreto (ECDH).
 *   3. POST /emparejar/confirmar?prueba=HMAC(código, "titiritero-emparejar\ncliente\n" + pubMóvil + "\n" + pubConsola)
 *      → {"id": N, "prueba": HMAC(código, "...\nconsola\n...")}: así cada lado sabe que el otro vio el código.
 *   La clave del aparato es SHA-256("titiritero-clave\n" + secreto + "\n" + pubMóvil + "\n" + pubConsola).
 *   Nunca viaja por la red: quien solo escucha no puede calcularla. */
#include <cstdint>

constexpr int MAX_DISPOSITIVOS = 8;
constexpr uint32_t ID_SERVIDOR = 0;   /* la clave.txt de la SD (instalación con servidor propio) */

struct Dispositivo {
    uint32_t id;           /* 0 = libre (salvo el servidor, que va aparte) */
    char nombre[40];
    char clave[65];        /* hex */
};

void dispositivos_iniciar();
/* Clave del aparato id, o nullptr si no existe. */
const char *dispositivos_clave(uint32_t id);
const char *dispositivos_nombre(uint32_t id);
/* Copia la lista (para el menú). Devuelve cuántos hay. */
int dispositivos_lista(Dispositivo *destino, int max);
void dispositivos_quitar(uint32_t id);
/* La clave del servidor (clave.txt); vacía = no hay servidor. */
void dispositivos_poner_clave_servidor(const char *clave);

/* Emparejamiento. El código lo crea el menú de ajustes al abrirse. */
void emparejar_nuevo_codigo(char *codigo, uint32_t cap);
bool emparejar_abierto();
int emparejar_segundos_restantes();
/* Paso 2. En *motivo, el error si falla. */
bool emparejar_iniciar(const char *pub_movil_hex, const char *nombre, char *pub_consola_hex, const char **motivo);
/* Paso 3. Devuelve el id nuevo (o 0) y la prueba de la consola. */
uint32_t emparejar_confirmar(const char *prueba_hex, char *prueba_consola_hex, const char **motivo);
