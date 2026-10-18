#pragma once
#include "http.h"

/* Cada orden recibe la petición ya autenticada y responde ella misma. */
typedef void (*Orden)(Peticion *p);

/* ordenes_sistema.cpp */
void orden_estado(Peticion *p);
void orden_info(Peticion *p);
void orden_registro(Peticion *p);
void orden_aviso(Peticion *p);
void orden_titulos(Peticion *p);
void orden_apps(Peticion *p);
void orden_abrir(Peticion *p);
void orden_titulo(Peticion *p);
void orden_menu(Peticion *p);
void orden_reiniciar(Peticion *p);
void orden_apagar(Peticion *p);
void orden_clave(Peticion *p);
void orden_olvidar(Peticion *p);
void orden_invitar(Peticion *p);

/* ordenes_mando.cpp */
void orden_pantalla(Peticion *p);
void orden_diagnostico(Peticion *p);
void orden_botones(Peticion *p);
void orden_tocar(Peticion *p);
void orden_palanca(Peticion *p);
void orden_vibrar(Peticion *p);

/* ordenes_archivos.cpp */
void orden_listar(Peticion *p);
void orden_bajar(Peticion *p);
void orden_subir(Peticion *p);
void orden_borrar(Peticion *p);
void orden_carpeta(Peticion *p);
/* Espacio libre y total de la SD en bytes. */
bool espacio_sd(uint64_t *libre, uint64_t *total);

/* Acción que el servidor ejecuta después de responder (cambiar de app cierra la actual). */
enum Despues { DESPUES_NADA, DESPUES_MENU, DESPUES_REINICIAR, DESPUES_APAGAR, DESPUES_CLAVE };
void ordenes_programar(Despues d);
Despues ordenes_pendiente();
