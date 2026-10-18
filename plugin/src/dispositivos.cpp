#include "dispositivos.h"
#include "comun.h"
#include "entropia.h"

#include <coreinit/mutex.h>
#include <coreinit/time.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <notifications/notifications.h>
#include <wups/storage.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
    constexpr int SEGUNDOS_CODIGO = 300;
    constexpr int INTENTOS = 5;

    OSMutex cerrojo;
    Dispositivo lista[MAX_DISPOSITIVOS];
    char clave_servidor[80];
    uint32_t siguiente_id = 1;

    struct Emparejamiento {
        char codigo[8];
        OSTime caduca;
        int intentos;
        int inicios;   /* cada inicio cuesta un cálculo de curva elíptica: máximo 10 por código */
        bool pendiente;
        char pub_movil[65], pub_consola[65], nombre[40];
        uint8_t secreto[32];
    } emp;

    struct Cerrojo {
        Cerrojo() { OSLockMutex(&cerrojo); }
        ~Cerrojo() { OSUnlockMutex(&cerrojo); }
    };

    /* ---------- utilidades ---------- */

    void a_hex(const uint8_t *b, uint32_t n, char *hex) {
        static const char D[] = "0123456789abcdef";
        for (uint32_t i = 0; i < n; ++i) hex[2 * i] = D[b[i] >> 4], hex[2 * i + 1] = D[b[i] & 15];
        hex[2 * n] = 0;
    }

    bool de_hex(const char *hex, uint8_t *b, uint32_t n) {
        if (strlen(hex) != 2 * n) return false;
        for (uint32_t i = 0; i < n; ++i) {
            char par[3] = {hex[2 * i], hex[2 * i + 1], 0};
            char *fin;
            b[i] = (uint8_t) strtoul(par, &fin, 16);
            if (*fin) return false;
        }
        return true;
    }

    void hmac_hex(const char *clave, const char *mensaje, char *hex) {
        uint8_t mac[32];
        mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t *) clave, strlen(clave),
                        (const uint8_t *) mensaje, strlen(mensaje), mac);
        a_hex(mac, 32, hex);
    }

    bool iguales(const char *a, const char *b) {
        size_t la = strlen(a), lb = strlen(b);
        unsigned d = la ^ lb;
        for (size_t i = 0; i < la && i < lb; ++i) d |= (unsigned char) a[i] ^ (unsigned char) b[i];
        return d == 0;
    }

    /* Nombre legible y seguro de guardar: sin tabuladores ni saltos, como mucho 39 bytes. */
    void limpiar_nombre(const char *entrada, char *salida) {
        uint32_t j = 0;
        for (const char *c = entrada; *c && j < 39; ++c)
            if ((unsigned char) *c >= 0x20 && *c != 0x7f) salida[j++] = *c;
        salida[j] = 0;
        if (!j) strcpy(salida, "Movil");
    }

    /* ---------- almacenamiento ---------- */

    void guardar() {
        for (int i = 0; i < MAX_DISPOSITIVOS; ++i) {
            char clave[12];
            snprintf(clave, sizeof clave, "disp%d", i);
            if (!lista[i].id) {
                WUPSStorageAPI::DeleteItem(clave);
                continue;
            }
            char valor[160];
            snprintf(valor, sizeof valor, "%u\t%s\t%s", (unsigned) lista[i].id, lista[i].nombre, lista[i].clave);
            WUPSStorageAPI::Store<std::string>(clave, valor);
        }
        WUPSStorageAPI::Store<uint32_t>("disp_siguiente", siguiente_id);
        WUPSStorageAPI::SaveStorage();
    }

    void cargar() {
        WUPSStorageAPI::GetOrStoreDefault<uint32_t>("disp_siguiente", siguiente_id, 1);
        for (int i = 0; i < MAX_DISPOSITIVOS; ++i) {
            char clave[12];
            snprintf(clave, sizeof clave, "disp%d", i);
            std::string v;
            memset(&lista[i], 0, sizeof lista[i]);
            if (WUPSStorageAPI::Get<std::string>(clave, v) != WUPS_STORAGE_ERROR_SUCCESS) continue;
            unsigned id;
            char nombre[40], k[65];
            if (sscanf(v.c_str(), "%u\t%39[^\t]\t%64s", &id, nombre, k) != 3 || !id || strlen(k) != 64) continue;
            lista[i].id = id;
            snprintf(lista[i].nombre, sizeof lista[i].nombre, "%s", nombre);
            snprintf(lista[i].clave, sizeof lista[i].clave, "%s", k);
        }
    }

    Dispositivo *hueco() {
        for (auto &d : lista)
            if (!d.id) return &d;
        return nullptr;
    }

    /* ---------- X25519 ---------- */

    /* Genera el par de la consola y calcula el secreto con la clave pública del móvil. */
    bool ecdh(const uint8_t *pub_movil, uint8_t *pub_consola, uint8_t *secreto) {
        mbedtls_ecp_group g;
        mbedtls_mpi d, z;
        mbedtls_ecp_point q, otro;
        mbedtls_ecp_group_init(&g);
        mbedtls_mpi_init(&d);
        mbedtls_mpi_init(&z);
        mbedtls_ecp_point_init(&q);
        mbedtls_ecp_point_init(&otro);
        uint8_t u[32];
        memcpy(u, pub_movil, 32);
        u[31] &= 0x7f;   /* RFC 7748: el bit alto se ignora */
        bool ok = mbedtls_ecp_group_load(&g, MBEDTLS_ECP_DP_CURVE25519) == 0 &&
                  mbedtls_ecdh_gen_public(&g, &d, &q, entropia_rng, nullptr) == 0 &&
                  mbedtls_mpi_write_binary_le(&q.X, pub_consola, 32) == 0 &&
                  mbedtls_mpi_read_binary_le(&otro.X, u, 32) == 0 && mbedtls_mpi_lset(&otro.Z, 1) == 0 &&
                  mbedtls_ecdh_compute_shared(&g, &z, &otro, &d, entropia_rng, nullptr) == 0 &&
                  mbedtls_mpi_write_binary_le(&z, secreto, 32) == 0;
        mbedtls_ecp_point_free(&otro);
        mbedtls_ecp_point_free(&q);
        mbedtls_mpi_free(&z);
        mbedtls_mpi_free(&d);
        mbedtls_ecp_group_free(&g);
        uint8_t o = 0;
        for (int i = 0; ok && i < 32; ++i) o |= secreto[i];
        return ok && o;   /* secreto todo a cero = clave pública de orden pequeño: rechazar */
    }

    void transcripcion(const char *quien, char *texto, uint32_t cap) {
        snprintf(texto, cap, "titiritero-emparejar\n%s\n%s\n%s", quien, emp.pub_movil, emp.pub_consola);
    }

    void cerrar_codigo() {
        memset(&emp, 0, sizeof emp);
    }
} // namespace

void dispositivos_iniciar() {
    OSInitMutex(&cerrojo);
    cargar();
}

void dispositivos_poner_clave_servidor(const char *clave) {
    Cerrojo c;
    snprintf(clave_servidor, sizeof clave_servidor, "%s", clave);
}

const char *dispositivos_clave(uint32_t id) {
    Cerrojo c;
    if (id == ID_SERVIDOR) return strlen(clave_servidor) >= 16 ? clave_servidor : nullptr;
    for (auto &d : lista)
        if (d.id == id) return d.clave;
    return nullptr;
}

const char *dispositivos_nombre(uint32_t id) {
    Cerrojo c;
    if (id == ID_SERVIDOR) return "servidor (clave.txt)";
    for (auto &d : lista)
        if (d.id == id) return d.nombre;
    return "?";
}

int dispositivos_lista(Dispositivo *destino, int max) {
    Cerrojo c;
    int n = 0;
    for (auto &d : lista)
        if (d.id && n < max) destino[n++] = d;
    return n;
}

void dispositivos_quitar(uint32_t id) {
    Cerrojo c;
    for (auto &d : lista)
        if (d.id == id) {
            registro("quitado el aparato %s", d.nombre);
            memset(&d, 0, sizeof d);
        }
    guardar();
}

void emparejar_nuevo_codigo(char *codigo, uint32_t cap) {
    Cerrojo c;
    cerrar_codigo();
    uint8_t r[4];
    if (!entropia_bytes(r, sizeof r)) return (void) snprintf(codigo, cap, "------");
    uint32_t n = ((uint32_t) r[0] << 24 | r[1] << 16 | r[2] << 8 | r[3]) % 1000000;
    snprintf(emp.codigo, sizeof emp.codigo, "%06u", (unsigned) n);
    emp.caduca = OSGetTime() + OSSecondsToTicks(SEGUNDOS_CODIGO);
    emp.intentos = INTENTOS;
    snprintf(codigo, cap, "%s", emp.codigo);
}

bool emparejar_abierto() {
    Cerrojo c;
    return emp.codigo[0] && OSGetTime() < emp.caduca && emp.intentos > 0;
}

int emparejar_segundos_restantes() {
    Cerrojo c;
    if (!emp.codigo[0]) return 0;
    OSTime t = OSGetTime();
    return t < emp.caduca ? (int) OSTicksToSeconds(emp.caduca - t) : 0;
}

bool emparejar_iniciar(const char *pub_movil_hex, const char *nombre, char *pub_consola_hex, const char **motivo) {
    if (!emparejar_abierto()) return *motivo = "no hay codigo activo: abre el menu de Titiritero en la consola", false;
    {
        Cerrojo c;
        if (++emp.inicios > 10) {
            cerrar_codigo();
            return *motivo = "demasiados intentos: vuelve a abrir el menu de Titiritero", false;
        }
    }
    uint8_t pm[32], pc[32], s[32];
    if (!de_hex(pub_movil_hex, pm, 32)) return *motivo = "clave publica no valida", false;
    if (!ecdh(pm, pc, s)) return *motivo = "no se pudo calcular el secreto (falta entropia o clave publica mala)", false;
    Cerrojo c;
    snprintf(emp.pub_movil, sizeof emp.pub_movil, "%s", pub_movil_hex);
    for (char *p = emp.pub_movil; *p; ++p)
        if (*p >= 'A' && *p <= 'F') *p += 32;
    a_hex(pc, 32, emp.pub_consola);
    memcpy(emp.secreto, s, 32);
    limpiar_nombre(nombre, emp.nombre);
    emp.pendiente = true;
    strcpy(pub_consola_hex, emp.pub_consola);
    return true;
}

uint32_t emparejar_confirmar(const char *prueba_hex, char *prueba_consola_hex, const char **motivo) {
    Cerrojo c;
    if (!emp.pendiente || !emp.codigo[0] || OSGetTime() >= emp.caduca)
        return *motivo = "el codigo caduco: vuelve a abrir el menu de Titiritero", 0u;
    char texto[220], esperada[65];
    transcripcion("cliente", texto, sizeof texto);
    hmac_hex(emp.codigo, texto, esperada);
    if (!iguales(esperada, prueba_hex)) {
        emp.pendiente = false;
        if (--emp.intentos <= 0) cerrar_codigo();
        registro("codigo de emparejamiento incorrecto");
        return *motivo = "codigo incorrecto", 0u;
    }
    Dispositivo *d = hueco();
    if (!d) return *motivo = "ya hay 8 aparatos: quita uno en el menu de Titiritero", 0u;
    /* Clave del aparato: SHA-256 de lo acordado (nunca ha viajado). */
    char semilla[300], sec_hex[65];
    a_hex(emp.secreto, 32, sec_hex);
    snprintf(semilla, sizeof semilla, "titiritero-clave\n%s\n%s\n%s", sec_hex, emp.pub_movil, emp.pub_consola);
    uint8_t k[32];
    mbedtls_sha256_ret((const uint8_t *) semilla, strlen(semilla), k, 0);
    d->id = siguiente_id++;
    snprintf(d->nombre, sizeof d->nombre, "%s", emp.nombre);
    a_hex(k, 32, d->clave);
    transcripcion("consola", texto, sizeof texto);
    hmac_hex(emp.codigo, texto, prueba_consola_hex);
    uint32_t id = d->id;
    char aviso_texto[96];
    snprintf(aviso_texto, sizeof aviso_texto, "Titiritero: emparejado \"%s\"", d->nombre);
    registro("emparejado %s (aparato %u)", d->nombre, (unsigned) id);
    cerrar_codigo();   /* un solo uso */
    guardar();
    NotificationModule_AddInfoNotification(aviso_texto);
    return id;
}
