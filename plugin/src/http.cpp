#include "http.h"

#include <sys/select.h>
#include <sys/socket.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

/* ---------- lectura ---------- */

bool http_esperar(int sock, int ms) {
    fd_set lectura;
    FD_ZERO(&lectura);
    FD_SET(sock, &lectura);
    timeval t = {ms / 1000, (ms % 1000) * 1000};
    return select(sock + 1, &lectura, nullptr, nullptr, &t) > 0;
}

static bool partir_linea(Peticion *p) {
    char destino[sizeof p->ruta + sizeof p->consulta];
    if (sscanf(p->buf, "%7s %831s", p->metodo, destino) != 2) return false;
    char *q = strchr(destino, '?');
    if (q) *q++ = 0;
    if (strlen(destino) >= sizeof p->ruta || (q && strlen(q) >= sizeof p->consulta)) return false;
    strcpy(p->ruta, destino);
    strcpy(p->consulta, q ? q : "");
    return true;
}

LecturaHttp http_leer_parte(Peticion *p) {
    if (p->leidos >= sizeof p->buf - 1) return HTTP_MAL;
    int n = recv(p->sock, p->buf + p->leidos, sizeof p->buf - 1 - p->leidos, 0);
    if (n <= 0) return HTTP_MAL;
    p->leidos += n;
    p->buf[p->leidos] = 0;
    char *fin = strstr(p->buf, "\r\n\r\n");
    if (!fin) return p->leidos >= sizeof p->buf - 1 ? HTTP_MAL : HTTP_FALTA;
    p->fin_cabecera = (uint32_t) (fin + 4 - p->buf);
    if (!partir_linea(p)) return HTTP_MAL;
    char largo[24];
    p->largo_cuerpo = http_cabecera(p, "Content-Length", largo, sizeof largo) ? strtoull(largo, nullptr, 10) : 0;
    return HTTP_LISTA;
}

bool http_cabecera(const Peticion *p, const char *nombre, char *valor, uint32_t cap) {
    size_t ln = strlen(nombre);
    for (const char *l = strstr(p->buf, "\r\n"); l && l[2] != '\r'; l = strstr(l + 2, "\r\n")) {
        const char *c = l + 2;
        if (strncasecmp(c, nombre, ln) || c[ln] != ':') continue;
        c += ln + 1;
        while (*c == ' ') ++c;
        uint32_t i = 0;
        while (c[i] && c[i] != '\r' && i < cap - 1) valor[i] = c[i], ++i;
        valor[i] = 0;
        return true;
    }
    return false;
}

static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void decodificar(const char *c, char *valor, uint32_t cap) {
    uint32_t i = 0;
    for (; *c && *c != '&' && i < cap - 1; ++c) {
        if (*c == '%' && hex(c[1]) >= 0 && hex(c[2]) >= 0) {
            valor[i++] = (char) (hex(c[1]) * 16 + hex(c[2]));
            c += 2;
        } else {
            valor[i++] = *c == '+' ? ' ' : *c;
        }
    }
    valor[i] = 0;
}

bool http_param(const Peticion *p, const char *nombre, char *valor, uint32_t cap) {
    size_t ln = strlen(nombre);
    for (const char *c = p->consulta; c && *c;) {
        if (!strncmp(c, nombre, ln) && c[ln] == '=') {
            decodificar(c + ln + 1, valor, cap);
            return true;
        }
        c = strchr(c, '&');
        if (c) ++c;
    }
    return false;
}

long http_param_num(const Peticion *p, const char *nombre, long defecto, long min, long max) {
    char v[24];
    if (!http_param(p, nombre, v, sizeof v)) return defecto;
    long n = strtol(v, nullptr, 10);
    return n < min ? min : n > max ? max : n;
}

/* ---------- respuesta ---------- */

bool http_enviar(int sock, const void *datos, uint32_t largo) {
    auto *p = (const uint8_t *) datos;
    while (largo) {
        int n = send(sock, p, largo > 32768 ? 32768 : largo, 0);
        if (n <= 0) return false;
        p += n;
        largo -= n;
    }
    return true;
}

static const char *texto_estado(int codigo) {
    switch (codigo) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        default: return "Error";
    }
}

void http_cabeceras(int sock, int codigo, const char *tipo, uint64_t largo) {
    char cab[256];
    int n = snprintf(cab, sizeof cab, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %llu\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                     codigo, texto_estado(codigo), tipo, (unsigned long long) largo);
    http_enviar(sock, cab, n);
}

void http_responder(int sock, int codigo, const char *tipo, const void *cuerpo, uint32_t largo) {
    http_cabeceras(sock, codigo, tipo, largo);
    if (largo) http_enviar(sock, cuerpo, largo);
}

void http_texto(int sock, int codigo, const char *fmt, ...) {
    char linea[320];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(linea, sizeof linea - 1, fmt, ap);
    va_end(ap);
    if (n > (int) sizeof linea - 2) n = sizeof linea - 2;
    linea[n++] = '\n';
    http_responder(sock, codigo, "text/plain; charset=utf-8", linea, n);
}

/* ---------- texto que crece ---------- */

static bool reservar(Texto *t, uint32_t extra) {
    if (t->sin_memoria) return false;
    if (t->n + extra + 1 <= t->cap) return true;
    uint32_t cap = t->cap ? t->cap : 1024;
    while (cap < t->n + extra + 1) cap *= 2;
    char *d = (char *) realloc(t->d, cap);
    if (!d) return !(t->sin_memoria = true);
    t->d = d;
    t->cap = cap;
    return true;
}

void tx_add(Texto *t, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    if (n < 0 || !reservar(t, n)) return;
    va_start(ap, fmt);
    vsnprintf(t->d + t->n, n + 1, fmt, ap);
    va_end(ap);
    t->n += n;
}

void tx_json(Texto *t, const char *s) {
    tx_add(t, "\"");
    for (; *s; ++s) {
        unsigned char c = (unsigned char) *s;
        if (c == '"' || c == '\\') tx_add(t, "\\%c", c);
        else if (c < 0x20) tx_add(t, "\\u%04x", c);
        else tx_add(t, "%c", c);
    }
    tx_add(t, "\"");
}

void tx_libre(Texto *t) {
    free(t->d);
    *t = {};
}

void http_json(int sock, Texto *t) {
    if (t->sin_memoria || !t->d) http_texto(sock, 500, "sin memoria para la respuesta");
    else http_responder(sock, 200, "application/json; charset=utf-8", t->d, t->n);
    tx_libre(t);
}
