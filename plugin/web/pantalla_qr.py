#!/usr/bin/env python3
"""Dibuja la pantalla de emparejamiento que la consola enseña con el QR.

Se genera aquí (con Pillow y la fuente Work Sans) y va dentro del plugin como JPEG, una por idioma. Lo que cambia
en cada consola (el QR, la dirección y el código) lo pinta el plugin encima, en los huecos que marca este script.
Salida: ../src/pantalla_qr.h. Con --muestra también deja PNG de prueba con datos inventados.
"""
import io, math, os, subprocess, sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

AQUI = os.path.dirname(os.path.abspath(__file__))
W, H = 1280, 720
NOCHE_ARRIBA, NOCHE_ABAJO = (0x2a, 0x1b, 0x4d), (0x12, 0x0b, 0x24)
TELON, HILO, HILO_SUAVE = (0x3b, 0x2a, 0x6b), (0xf1, 0xe6, 0xcf), (0xc9, 0xbf, 0xd9)
MADERA, MADERA_OSCURA, MANDO = (0xe9, 0xa2, 0x3b), (0x8a, 0x4a, 0x14), (0xfb, 0xfa, 0xf6)

# Huecos que rellena el plugin (en coordenadas de 1280x720)
TARJETA = (770, 168, 410, 410)           # x, y, ancho, alto
QR = (790, 188, 370, 370)                # zona del QR dentro de la tarjeta
DIRECCION = (96, 556)                    # esquina superior izquierda del texto
CODIGO = (96, 622)

TEXTOS = {
    'es': dict(sub='Conecta tu móvil a esta Wii U',
               pasos=['Abre la cámara del móvil', 'Apunta a este código QR', 'Toca el enlace que aparece'],
               wifi='El móvil tiene que estar en la misma red Wi-Fi.',
               otra='¿No lee el QR? Escribe esto en el navegador y luego el código:',
               pie='El código vale 5 minutos  ·  Pulsa B para cerrar'),
    'en': dict(sub='Connect your phone to this Wii U',
               pasos=['Open your phone’s camera', 'Point it at this QR code', 'Tap the link that appears'],
               wifi='Your phone must be on the same Wi-Fi network.',
               otra='Can’t scan it? Type this in the browser, then the code:',
               pie='The code lasts 5 minutes  ·  Press B to close'),
}


def fuente(peso, tam):
    nombre = 'WorkSans_600SemiBold.ttf' if peso == 600 else 'WorkSans_400Regular.ttf'
    return ImageFont.truetype(os.path.join(AQUI, 'fuentes', nombre), tam)


def fondo():
    img = Image.new('RGB', (W, H))
    d = ImageDraw.Draw(img)
    for y in range(H):
        t = y / (H - 1)
        d.line([(0, y), (W, y)], fill=tuple(round(a + (b - a) * t) for a, b in zip(NOCHE_ARRIBA, NOCHE_ABAJO)))
    # telón: un resplandor suave detrás de la tarjeta
    luz = Image.new('L', (W, H), 0)
    ImageDraw.Draw(luz).ellipse([TARJETA[0] - 170, TARJETA[1] - 120, TARJETA[0] + TARJETA[2] + 170, TARJETA[1] + TARJETA[3] + 140], fill=115)
    luz = luz.filter(ImageFilter.GaussianBlur(70))
    return Image.composite(Image.new('RGB', (W, H), TELON), img, luz)


def marioneta(img):
    """La tarjeta del QR cuelga de la cruceta con dos hilos: es el sello del icono."""
    x, y, w, h = TARJETA
    cx = x + w // 2
    d = ImageDraw.Draw(img)
    barra_y = 82
    extremos = [(cx - 150, barra_y + 11), (cx + 150, barra_y + 11)]
    anclas = [(x + 40, y + 2), (x + w - 40, y + 2)]
    for a, b in zip(extremos, anclas):
        d.line([a, b], fill=HILO, width=3)
    # sombra y tarjeta
    sombra = Image.new('L', (W, H), 0)
    ImageDraw.Draw(sombra).rounded_rectangle([x + 4, y + 14, x + w + 4, y + h + 18], 30, fill=150)
    sombra = sombra.filter(ImageFilter.GaussianBlur(18))
    img.paste(Image.new('RGB', (W, H), (8, 4, 18)), (0, 0), sombra)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([x, y, x + w, y + h], 28, fill=MANDO)
    for ax, ay in anclas:
        d.ellipse([ax - 6, ay - 6, ax + 6, ay + 6], fill=MADERA_OSCURA)
    # cruceta: barra horizontal y vertical de madera
    d.rounded_rectangle([cx - 170, barra_y, cx + 170, barra_y + 22], 11, fill=MADERA, outline=MADERA_OSCURA, width=3)
    d.rounded_rectangle([cx - 11, barra_y - 44, cx + 11, barra_y + 62], 11, fill=MADERA, outline=MADERA_OSCURA, width=3)
    for ex, ey in extremos:
        d.ellipse([ex - 5, ey - 5, ex + 5, ey + 5], fill=MADERA_OSCURA)


def textos(img, t):
    d = ImageDraw.Draw(img)
    d.text((96, 88), 'Titiritero', font=fuente(600, 64), fill=HILO)
    d.text((98, 172), t['sub'], font=fuente(400, 29), fill=MADERA)
    for i, paso in enumerate(t['pasos']):
        y = 244 + i * 58
        d.ellipse([96, y, 136, y + 40], fill=MADERA)
        d.text((116, y + 20), str(i + 1), font=fuente(600, 22), fill=NOCHE_ABAJO, anchor='mm')
        d.text((156, y + 20), paso, font=fuente(400, 27), fill=HILO, anchor='lm')
    d.text((96, 428), t['wifi'], font=fuente(400, 20), fill=HILO_SUAVE)
    d.line([(96, 486), (660, 486)], fill=TELON, width=2)
    d.text((96, 510), t['otra'], font=fuente(400, 19), fill=HILO_SUAVE)
    x, y, w, h = TARJETA
    d.text((x + w // 2, y + h + 44), t['pie'], font=fuente(400, 19), fill=HILO_SUAVE, anchor='mm')


def plantilla(lang):
    img = fondo()
    marioneta(img)
    textos(img, TEXTOS[lang])
    return img


def atlas(nombre, peso, tam, color, caracteres):
    """Cada carácter en una celda del alto de la línea; el plugin las pone una detrás de otra."""
    f = fuente(peso, tam)
    asc, desc = f.getmetrics()
    alto = asc + desc
    celdas = []
    for c in caracteres:
        ancho = max(1, math.ceil(f.getlength(c)))
        cel = Image.new('L', (ancho, alto), 0)
        ImageDraw.Draw(cel).text((0, 0), c, font=f, fill=255)
        celdas.append((c, ancho, cel.tobytes()))
    datos = b''.join(c[2] for c in celdas)
    lineas = [f'/* {nombre}: Work Sans {peso} {tam}px, color {color} */',
              f'static const uint32_t {nombre}_ALTO = {alto};',
              f'static const uint32_t {nombre}_COLOR = 0x{color[0]:02X}{color[1]:02X}{color[2]:02X}FF;',
              f'static const char {nombre}_CARACTERES[] = "{caracteres}";']
    desplaz, pos = [], 0
    for _, ancho, b in celdas:
        desplaz.append(pos); pos += len(b)
    lineas.append(f'static const uint16_t {nombre}_ANCHO[] = {{{", ".join(str(c[1]) for c in celdas)}}};')
    lineas.append(f'static const uint32_t {nombre}_POS[] = {{{", ".join(map(str, desplaz))}}};')
    lineas.append(f'static const uint8_t {nombre}_ALFA[] = {{{bytes_c(datos)}}};')
    return lineas, f, celdas, alto


def bytes_c(b):
    return ','.join(str(x) for x in b)


def jpeg(img):
    buf = io.BytesIO()
    img.save(buf, 'JPEG', quality=93, subsampling=0, optimize=True)
    return buf.getvalue()


def main():
    salida = ['/* Generado por web/pantalla_qr.py: no editar a mano. */', '#pragma once', '#include <cstdint>', '',
              f'static const uint32_t PQR_ANCHO = {W}, PQR_ALTO = {H};',
              f'static const uint32_t PQR_TARJETA[4] = {{{", ".join(map(str, TARJETA))}}};',
              f'static const uint32_t PQR_QR[4] = {{{", ".join(map(str, QR))}}};',
              f'static const uint32_t PQR_DIRECCION[2] = {{{DIRECCION[0]}, {DIRECCION[1]}}};',
              f'static const uint32_t PQR_CODIGO[2] = {{{CODIGO[0]}, {CODIGO[1]}}};', '']
    for lang in ('es', 'en'):
        img = plantilla(lang)
        j = jpeg(img)
        salida.append(f'static const uint8_t PQR_JPEG_{lang.upper()}[] = {{{bytes_c(j)}}};')
        if '--muestra' in sys.argv:
            img.save(os.path.join(AQUI, f'muestra_{lang}.png'))
        print(f'plantilla {lang}: {len(j)} bytes', file=sys.stderr)
    l1, *_ = atlas('PQR_LETRA_DIR', 600, 38, HILO, '0123456789.:/hpt')
    l2, *_ = atlas('PQR_LETRA_COD', 600, 64, MADERA, '0123456789 ')
    salida += [''] + l1 + [''] + l2
    with open(os.path.join(AQUI, '..', 'src', 'pantalla_qr.h'), 'w') as fh:
        fh.write('\n'.join(salida) + '\n')


if __name__ == '__main__':
    main()
