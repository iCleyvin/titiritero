# Imagen para compilar el plugin: mismas piezas que los plugins oficiales de wiiu-env,
# fijadas por digest (etiqueta entre paréntesis solo como referencia).
# (devkitppc:20260504)
FROM ghcr.io/wiiu-env/devkitppc@sha256:68785e7d75b070b0aea03272b61f36dd2fddfda4f442cdf0e27b168bd36a09fb
# (wiiupluginsystem:20260418)
COPY --from=ghcr.io/wiiu-env/wiiupluginsystem@sha256:91fe2c0b1fce10cb3744c670e57b161d4dd97f8c72cae7c5f9fb0ee2c69811ad /artifacts $DEVKITPRO
# (libmappedmemory:20260331)
COPY --from=ghcr.io/wiiu-env/libmappedmemory@sha256:666d10caeb0dc2f27f923458cbd5424d51cf11f96aa060038d61a258aa53f1b5 /artifacts $DEVKITPRO
# (libnotifications:20260404)
COPY --from=ghcr.io/wiiu-env/libnotifications@sha256:bb43d7fae8c02dd7bb3e58b103a82407a9528c2221ab7f5928dada3ec79378f8 /artifacts $DEVKITPRO
# (librpxloader:20260329)
COPY --from=ghcr.io/wiiu-env/librpxloader@sha256:b77cf29f0759ed9bca4a4f0ed54bee1af61b2cc292457c2fb65f09c9b2f7304b /artifacts $DEVKITPRO
WORKDIR /project
