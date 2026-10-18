# Avisos y créditos

Titiritero se publica con la licencia **GPL-3.0** (archivo `LICENSE`).

Usa o se basa en el trabajo de otras personas:

| Qué | De quién | Licencia | Dónde se usa |
|---|---|---|---|
| Método para capturar la pantalla | [ScreenshotWUPS](https://github.com/wiiu-env/ScreenshotWUPS), de Maschell | GPL-3.0 | `plugin/src/captura.cpp` |
| QR Code generator library | [Project Nayuki](https://www.nayuki.io/page/qr-code-generator-library) | MIT (aviso dentro de `qrcodegen.c` y `qrcodegen.h`) | `plugin/src/qrcodegen.*` |
| Fuente Work Sans | [The Work Sans Project Authors](https://github.com/weiweihuanghuang/Work-Sans) | SIL Open Font License 1.1 (`plugin/web/fuentes/OFL.txt`) | Pantalla del QR en la consola |
| wut, WUPS (sistema de plugins), libmappedmemory, libnotifications, librpxloader | [devkitPro](https://github.com/devkitPro/wut) y [wiiu-env](https://github.com/wiiu-env) | Las de cada proyecto | Para compilar y en la consola |
| mbedTLS (criptografía) | Proyecto Mbed TLS | Apache-2.0 | Firmas y emparejamiento |
| libjpeg-turbo | Proyecto libjpeg-turbo | BSD / IJG | Capturas en JPEG |

Wii U, GamePad y Nintendo Network son marcas de Nintendo. Titiritero es un proyecto independiente,
sin relación con Nintendo.
