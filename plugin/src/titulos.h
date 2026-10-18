#pragma once
#include "http.h"
#include <cstdint>

/* Juegos instalados (y Virtual Console), con su nombre en español si lo hay. Guardados tras la primera vez. */
void titulos_json(Texto *t, bool fresca);
void titulos_calcular(Texto *t);
/* Homebrew de la SD: wiiu/apps/<carpeta>/<x>.wuhb|rpx y wiiu/apps/<x>.wuhb|rpx. */
void apps_json(Texto *t);
/* Nombre del título en español (o inglés); false si el sistema no lo da. */
bool nombre_titulo(uint64_t id, char *nombre, uint32_t cap);
