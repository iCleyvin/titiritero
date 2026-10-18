#pragma once
/* HTTP/1.1 mínimo: una petición por conexión, cabeceras hasta 4 KB, cuerpo leído aparte. */
#include <cstdint>

struct Peticion {
    int sock;
    uint32_t ip;               /* del cliente, en orden de red */
    uint32_t disp;             /* aparato que firmó la orden (tras comprobar la firma) */
    char metodo[8];
    char ruta[192];
    char consulta[640];        /* lo que va tras '?', sin decodificar */
    char buf[4096];
    uint32_t leidos;           /* bytes en buf */
    uint32_t fin_cabecera;     /* índice del primer byte del cuerpo */
    uint64_t largo_cuerpo;     /* Content-Length, 0 si no hay */
};

/* Lee lo que haya llegado (llamar cuando select diga que hay datos). LISTA: cabecera entera y analizada. */
enum LecturaHttp { HTTP_FALTA, HTTP_LISTA, HTTP_MAL };
LecturaHttp http_leer_parte(Peticion *p);
bool http_cabecera(const Peticion *p, const char *nombre, char *valor, uint32_t cap);
bool http_param(const Peticion *p, const char *nombre, char *valor, uint32_t cap);
long http_param_num(const Peticion *p, const char *nombre, long defecto, long min, long max);

/* Espera hasta ms a que haya datos que leer en el socket. */
bool http_esperar(int sock, int ms);
bool http_enviar(int sock, const void *datos, uint32_t largo);
void http_cabeceras(int sock, int codigo, const char *tipo, uint64_t largo);
void http_responder(int sock, int codigo, const char *tipo, const void *cuerpo, uint32_t largo);
void http_texto(int sock, int codigo, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* Texto que crece (para respuestas JSON de tamaño variable). */
struct Texto {
    char *d;
    uint32_t n, cap;
    bool sin_memoria;
};
void tx_add(Texto *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Añade s como cadena JSON, con comillas y escapes. */
void tx_json(Texto *t, const char *s);
void tx_libre(Texto *t);
/* Responde con t como JSON (o 500 si faltó memoria) y lo libera. */
void http_json(int sock, Texto *t);
