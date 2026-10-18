#pragma once
/* Español si la consola está en español; inglés en cualquier otro idioma. */
void idioma_iniciar();
bool idioma_es();
/* Devuelve el texto en el idioma de la consola. */
inline const char *tr(const char *es, const char *en) { return idioma_es() ? es : en; }
