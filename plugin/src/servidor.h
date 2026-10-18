#pragma once

/* Servidor HTTP mínimo en TITIRITERO_PUERTO. Vive mientras dure cada app:
 * se arranca en ON_APPLICATION_START y se para en ON_APPLICATION_ENDS. */
void servidor_arrancar();
void servidor_parar();
/* true mientras la app se está cerrando: las esperas largas deben cortarse. */
bool servidor_parando();
