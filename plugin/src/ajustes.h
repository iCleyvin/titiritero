#pragma once
/* Ajustes del plugin: se cambian en el menú de plugins de Aroma (L + abajo + SELECT) y se guardan
 * con el almacenamiento de WUPS. El servidor los consulta en cada orden. */
#include <cstdint>

enum Permiso {
    PERMISO_SIEMPRE,     /* estado, info, listas: nunca se bloquean */
    PERMISO_VER,         /* capturas de pantalla */
    PERMISO_MANDOS,      /* botones, táctil, palancas */
    PERMISO_ABRIR,       /* abrir juegos y apps */
    PERMISO_ENERGIA,     /* menú, reiniciar, apagar */
    PERMISO_AVISOS,      /* mensajes y vibración */
    PERMISO_LEER,        /* listar y descargar archivos */
    PERMISO_ESCRIBIR,    /* subir, borrar, crear carpetas */
    PERMISO_ADMIN,       /* cambiar la clave: siempre que el control remoto no esté en pausa */
};

enum Archivos { ARCHIVOS_NO = 0, ARCHIVOS_LEER = 1, ARCHIVOS_TODO = 2 };

void ajustes_iniciar();
void ajustes_cerrar();
/* Aviso corto la primera vez que arranca una app tras encender (si los avisos están activados). */
void ajustes_bienvenida();

bool ajustes_servidor_activo();
bool ajustes_permite(Permiso p);
/* Límite de imágenes por segundo de la vista en vivo. */
uint32_t ajustes_fps();

/* El servidor la llama tras cada orden aceptada: guarda qué aparato y cuándo, y avisa en pantalla
 * si es el comienzo de una sesión de control (y el aviso está activado). */
void ajustes_actividad(uint32_t disp, const char *ruta);

/* Muestra el QR de emparejamiento en pantalla (con un código nuevo si se pide desde el panel). */
void ajustes_mostrar_qr(bool codigo_nuevo);
