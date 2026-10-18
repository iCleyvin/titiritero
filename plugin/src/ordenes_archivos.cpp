/* Gestor de archivos de la SD por HTTP: hace innecesario el FTP sin contraseña de ftpiiu. */
#include "comun.h"
#include "servidor.h"
#include "firma.h"
#include "ordenes.h"

#include <coreinit/filesystem_fsa.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace {
    /* Hasta 15 s por trozo, pero se corta en cuanto la app empieza a cerrarse (la consola espera a este hilo). */
    bool esperar_datos(int sock) {
        for (int i = 0; i < 60; ++i) {
            if (servidor_parando()) return false;
            if (http_esperar(sock, 250)) return true;
        }
        return false;
    }

    constexpr const char *RAIZ_SD = "fs:/vol/external01/";
    constexpr uint64_t MAX_SUBIDA = 512ull * 1024 * 1024;
    constexpr uint32_t TROZO = 64 * 1024;

    /* Deja la ruta en su forma única: sin barras repetidas, sin "." y sin barra al principio ni al final.
     * Rechaza "..", "\\", ":" y "~" (con "~" la SD acepta el nombre corto de un archivo, p. ej. TITIRI~1.JSO,
     * que esquivaría las comprobaciones por nombre). */
    bool normalizar(const char *rel, char *dst, uint32_t cap) {
        if (strpbrk(rel, "\\:~")) return false;
        uint32_t n = 0;
        for (const char *c = rel; *c;) {
            while (*c == '/') ++c;
            const char *fin = strchrnul(c, '/');
            uint32_t largo = (uint32_t) (fin - c);
            if (largo == 2 && c[0] == '.' && c[1] == '.') return false;
            /* FAT ignora puntos y espacios al final ("titiritero." es "titiritero"): fuera, para que no esquiven
             * las comprobaciones por nombre. Tampoco caracteres de control. */
            if (largo > 1 && (c[largo - 1] == '.' || c[largo - 1] == ' ')) return false;
            for (uint32_t i = 0; i < largo; ++i)
                if ((unsigned char) c[i] < 0x20) return false;
            if (largo && !(largo == 1 && c[0] == '.')) {
                if (n + largo + 2 > cap) return false;
                if (n) dst[n++] = '/';
                memcpy(dst + n, c, largo);
                n += largo;
            }
            c = fin;
        }
        dst[n] = 0;
        return true;
    }

    /* Secretos que no salen ni se tocan por HTTP: la clave del servidor y los ajustes del plugin, donde Aroma
     * guarda las claves de los móviles emparejados. Quien los tuviera seguiría entrando aunque lo quitaran. */
    bool protegida(const char *r) {
        if (!strncasecmp(r, "wiiu/titiritero", 15) && (!r[15] || r[15] == '/')) return true;   /* clave y permitidos.txt */
        if (strncasecmp(r, "wiiu/environments/", 18)) return false;
        const char *resto = strchr(r + 18, '/');
        return resto && !strncasecmp(resto, "/plugins/config/titiritero", 26);
    }

    /* Convierte una ruta relativa a la SD en absoluta. */
    bool ruta_sd(const Peticion *p, char *abs, uint32_t cap) {
        char rel[400], limpia[400];
        if (!http_param(p, "ruta", rel, sizeof rel)) rel[0] = 0;
        if (!normalizar(rel, limpia, sizeof limpia) || protegida(limpia)) return false;
        int n = snprintf(abs, cap, "%s%s", RAIZ_SD, limpia);
        return n > 0 && n < (int) cap;
    }

    bool ruta_o_error(Peticion *p, char *abs, uint32_t cap) {
        if (ruta_sd(p, abs, cap)) return true;
        http_texto(p->sock, 400, "ruta no valida o protegida");
        return false;
    }

    /* mkdir -p de todas las carpetas que contiene la ruta (no de la última pieza). */
    void crear_padres(const char *abs) {
        char tmp[600];
        snprintf(tmp, sizeof tmp, "%s", abs);
        for (char *c = tmp + strlen(RAIZ_SD); *c; ++c) {
            if (*c != '/') continue;
            *c = 0;
            mkdir(tmp, 0777);
            *c = '/';
        }
    }

    void entrada_json(Texto *t, const char *carpeta, const char *nombre, bool primero) {
        char completa[800];
        snprintf(completa, sizeof completa, "%s/%s", carpeta, nombre);
        struct stat st = {};
        bool es_dir = stat(completa, &st) == 0 && S_ISDIR(st.st_mode);
        tx_add(t, "%s{\"nombre\":", primero ? "" : ",");
        tx_json(t, nombre);
        tx_add(t, ",\"carpeta\":%s,\"bytes\":%llu}", es_dir ? "true" : "false",
               es_dir ? 0ull : (unsigned long long) st.st_size);
    }

    void espacio_json(Texto *t) {
        uint64_t libre, total;
        if (espacio_sd(&libre, &total)) tx_add(t, ",\"libre\":%llu,\"total\":%llu", libre, total);
    }

    /* Recibe el cuerpo, lo escribe en f y comprueba que su SHA-256 es el firmado en X-Cuerpo. */
    bool recibir_cuerpo(Peticion *p, FILE *f, const char **motivo) {
        char declarado[72] = "";
        http_cabecera(p, "X-Cuerpo", declarado, sizeof declarado);
        Sha256 *h = sha256_nuevo();
        uint64_t falta = p->largo_cuerpo;
        uint32_t ya = p->leidos - p->fin_cabecera;
        if (ya > falta) ya = (uint32_t) falta;
        bool ok = !ya || fwrite(p->buf + p->fin_cabecera, 1, ya, f) == ya;
        sha256_sumar(h, p->buf + p->fin_cabecera, ya);
        falta -= ya;
        static uint8_t trozo[TROZO];
        while (ok && falta) {
            int n = esperar_datos(p->sock) ? recv(p->sock, trozo, falta < TROZO ? (int) falta : (int) TROZO, 0) : -1;
            ok = n > 0 && fwrite(trozo, 1, n, f) == (size_t) n;
            if (ok) sha256_sumar(h, trozo, n), falta -= n;
        }
        char real[65];
        sha256_fin(h, real);
        if (!ok) return *motivo = "subida incompleta", false;
        if (strcmp(real, declarado)) return *motivo = "el archivo no coincide con el firmado (alterado por el camino)", false;
        return true;
    }

    /* Escribe a disco de verdad: la Wii U guarda en caché y un cuelgue perdería el archivo. */
    bool cerrar_sincronizado(FILE *f) {
        bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
        return fclose(f) == 0 && ok;
    }
} // namespace

/* statvfs de wut devuelve ENOSYS en la SD a propósito: se pregunta directamente a FSA. */
bool espacio_sd(uint64_t *libre, uint64_t *total) {
    FSAInit();
    FSAClientHandle cliente = FSAAddClient(nullptr);
    if (cliente < 0) return false;
    FSADeviceInfo disp = {};
    bool ok = FSAGetFreeSpaceSize(cliente, "/vol/external01", libre) >= 0 &&
              FSAGetDeviceInfo(cliente, "/vol/external01", &disp) >= 0;
    FSADelClient(cliente);
    *total = disp.deviceSizeInSectors * disp.deviceSectorSize;
    return ok;
}

void orden_listar(Peticion *p) {
    char abs[512];
    if (!ruta_o_error(p, abs, sizeof abs)) return;
    DIR *d = opendir(abs);
    if (!d) return http_texto(p->sock, 404, "no existe esa carpeta");
    Texto t = {};
    tx_add(&t, "{\"ruta\":");
    tx_json(&t, abs + strlen(RAIZ_SD));
    espacio_json(&t);
    tx_add(&t, ",\"entradas\":[");
    bool primero = true;
    for (dirent *e = readdir(d); e; e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        entrada_json(&t, abs, e->d_name, primero);
        primero = false;
    }
    closedir(d);
    tx_add(&t, "]}");
    http_json(p->sock, &t);
}

void orden_bajar(Peticion *p) {
    char abs[512];
    if (!ruta_o_error(p, abs, sizeof abs)) return;
    struct stat st = {};
    if (stat(abs, &st) != 0 || S_ISDIR(st.st_mode)) return http_texto(p->sock, 404, "no existe ese archivo");
    FILE *f = fopen(abs, "rb");
    if (!f) return http_texto(p->sock, 500, "no se pudo abrir");
    http_cabeceras(p->sock, 200, "application/octet-stream", st.st_size);
    static uint8_t trozo[TROZO];
    for (size_t n; (n = fread(trozo, 1, TROZO, f)) > 0;)
        if (!http_enviar(p->sock, trozo, n)) break;
    fclose(f);
}

void orden_subir(Peticion *p) {
    char abs[512], tmp[540];
    if (!ruta_o_error(p, abs, sizeof abs)) return;
    if (p->largo_cuerpo > MAX_SUBIDA) return http_texto(p->sock, 413, "maximo 512 MB");
    crear_padres(abs);
    snprintf(tmp, sizeof tmp, "%s.titiritero-tmp", abs);
    FILE *f = fopen(tmp, "wb");
    if (!f) return http_texto(p->sock, 500, "no se pudo crear el archivo (errno %d)", errno);
    setvbuf(f, nullptr, _IOFBF, 128 * 1024);
    const char *motivo = "no se pudo escribir en la SD";
    bool ok = recibir_cuerpo(p, f, &motivo);
    ok = cerrar_sincronizado(f) && ok;
    if (!ok) {
        remove(tmp);
        return http_texto(p->sock, 500, "%s; no se ha tocado el archivo original", motivo);
    }
    remove(abs);   /* FAT no renombra encima de un archivo existente */
    if (rename(tmp, abs) != 0) return http_texto(p->sock, 500, "no se pudo renombrar (errno %d)", errno);
    registro("subido %s (%llu bytes)", abs + strlen(RAIZ_SD), (unsigned long long) p->largo_cuerpo);
    http_texto(p->sock, 200, "%llu", (unsigned long long) p->largo_cuerpo);
}

void orden_borrar(Peticion *p) {
    char abs[512];
    if (!ruta_o_error(p, abs, sizeof abs)) return;
    if (strlen(abs) <= strlen(RAIZ_SD)) return http_texto(p->sock, 400, "no se borra la raiz de la SD");
    struct stat st = {};
    if (stat(abs, &st) != 0) return http_texto(p->sock, 404, "no existe");
    int r = S_ISDIR(st.st_mode) ? rmdir(abs) : remove(abs);
    if (r != 0) return http_texto(p->sock, 409, "no se pudo borrar (una carpeta tiene que estar vacia)");
    registro("borrado %s", abs + strlen(RAIZ_SD));
    http_texto(p->sock, 200, "borrado");
}

void orden_carpeta(Peticion *p) {
    char abs[512];
    if (!ruta_o_error(p, abs, sizeof abs)) return;
    char con_barra[520];
    snprintf(con_barra, sizeof con_barra, "%s/", abs);
    crear_padres(con_barra);
    struct stat st = {};
    if (stat(abs, &st) != 0 || !S_ISDIR(st.st_mode)) return http_texto(p->sock, 500, "no se pudo crear");
    http_texto(p->sock, 200, "creada");
}
