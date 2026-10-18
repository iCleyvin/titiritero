#pragma once
#include <cstdint>
#include <string>

/* Acciones sobre la consola. Las que cambian de app hacen que la actual se cierre. */

void sistema_iniciar();
void sistema_cerrar();

/* Abre un .wuhb o .rpx; ruta relativa a la raíz de la SD (p. ej. "wiiu/apps/x/x.wuhb"). */
bool sistema_abrir(const char *ruta, const char **motivo);
/* Abre un título instalado por su id (16 cifras hexadecimales). */
bool sistema_abrir_titulo(uint64_t id, const char **motivo);
void sistema_menu();
void sistema_reiniciar();
/* Apaga la consola. Desde el servidor no se puede volver a encender. */
void sistema_apagar();
/* true si hay algo escuchando en el puerto 21 de la consola (ftpiiu): da la SD sin contraseña a toda la red. */
bool sistema_ftp_abierto();
/* IP de la consola en la red de casa ("sin red" si no tiene). */
std::string sistema_ip();
