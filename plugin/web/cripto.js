/* Criptografía del panel, escrita a mano: la página se sirve por HTTP desde la consola y en una página
 * sin HTTPS el navegador no da crypto.subtle. Solo se usa crypto.getRandomValues, que sí está.
 * SHA-256 (FIPS 180-4), HMAC-SHA256 (RFC 2104) y X25519 (RFC 7748, con BigInt). */
const Cripto = (() => {
  const K = new Uint32Array([
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2]);
  const utf8 = s => new TextEncoder().encode(s);
  const hex = b => Array.from(b, x => x.toString(16).padStart(2, "0")).join("");
  const deHex = h => new Uint8Array(h.match(/../g).map(x => parseInt(x, 16)));

  /* SHA-256 por trozos: sirve para archivos grandes sin copiarlos enteros. */
  function Sha256() {
    const H = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
    const W = new Uint32Array(64), bloque = new Uint8Array(64);
    let usado = 0, total = 0;
    const rotr = (x, n) => (x >>> n) | (x << (32 - n));
    function procesar(b, o) {
      for (let i = 0; i < 16; i++) W[i] = (b[o + 4 * i] << 24) | (b[o + 4 * i + 1] << 16) | (b[o + 4 * i + 2] << 8) | b[o + 4 * i + 3];
      for (let i = 16; i < 64; i++) {
        const s0 = rotr(W[i - 15], 7) ^ rotr(W[i - 15], 18) ^ (W[i - 15] >>> 3);
        const s1 = rotr(W[i - 2], 17) ^ rotr(W[i - 2], 19) ^ (W[i - 2] >>> 10);
        W[i] = (W[i - 16] + s0 + W[i - 7] + s1) | 0;
      }
      let [a, bb, c, d, e, f, g, h] = H;
      for (let i = 0; i < 64; i++) {
        const t1 = (h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + W[i]) | 0;
        const t2 = ((rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c))) | 0;
        h = g; g = f; f = e; e = (d + t1) | 0; d = c; c = bb; bb = a; a = (t1 + t2) | 0;
      }
      H[0] += a; H[1] += bb; H[2] += c; H[3] += d; H[4] += e; H[5] += f; H[6] += g; H[7] += h;
    }
    return {
      sumar(datos) {
        let i = 0;
        total += datos.length;
        if (usado) {
          while (usado < 64 && i < datos.length) bloque[usado++] = datos[i++];
          if (usado < 64) return this;
          procesar(bloque, 0); usado = 0;
        }
        for (; i + 64 <= datos.length; i += 64) procesar(datos, i);
        while (i < datos.length) bloque[usado++] = datos[i++];
        return this;
      },
      fin() {
        const bits = total * 8, relleno = new Uint8Array(((usado < 56 ? 56 : 120) - usado) + 8);
        relleno[0] = 0x80;
        const v = new DataView(relleno.buffer);
        v.setUint32(relleno.length - 8, Math.floor(bits / 2 ** 32));
        v.setUint32(relleno.length - 4, bits >>> 0);
        total -= relleno.length;   /* el relleno no cuenta en la longitud */
        this.sumar(relleno);
        const salida = new Uint8Array(32);
        const vs = new DataView(salida.buffer);
        H.forEach((x, i) => vs.setUint32(4 * i, x));
        return salida;
      },
    };
  }
  const sha256 = datos => Sha256().sumar(typeof datos === "string" ? utf8(datos) : datos).fin();

  function hmac(clave, mensaje) {
    let k = typeof clave === "string" ? utf8(clave) : clave;
    if (k.length > 64) k = sha256(k);
    const ki = new Uint8Array(64), ko = new Uint8Array(64);
    for (let i = 0; i < 64; i++) { ki[i] = (k[i] || 0) ^ 0x36; ko[i] = (k[i] || 0) ^ 0x5c; }
    const dentro = Sha256().sumar(ki).sumar(typeof mensaje === "string" ? utf8(mensaje) : mensaje).fin();
    return Sha256().sumar(ko).sumar(dentro).fin();
  }

  /* X25519 (RFC 7748, escalera de Montgomery). */
  const P = 2n ** 255n - 19n, A24 = 121665n;
  const md = a => { a %= P; return a < 0n ? a + P : a; };
  const potencia = (b, e) => { let r = 1n; b = md(b); while (e > 0n) { if (e & 1n) r = r * b % P; b = b * b % P; e >>= 1n; } return r; };
  const aEntero = b => { let n = 0n; for (let i = b.length - 1; i >= 0; i--) n = (n << 8n) | BigInt(b[i]); return n; };
  const aBytes = n => { const o = new Uint8Array(32); for (let i = 0; i < 32; i++) { o[i] = Number(n & 255n); n >>= 8n; } return o; };
  function x25519(escalar, u) {
    const k = escalar.slice(); k[0] &= 248; k[31] &= 127; k[31] |= 64;
    const ub = u.slice(); ub[31] &= 127;
    const kn = aEntero(k), x1 = aEntero(ub) % P;
    let x2 = 1n, z2 = 0n, x3 = x1, z3 = 1n, cambio = 0n;
    for (let t = 254; t >= 0; t--) {
      const kt = (kn >> BigInt(t)) & 1n;
      cambio ^= kt;
      if (cambio) { [x2, x3] = [x3, x2]; [z2, z3] = [z3, z2]; }
      cambio = kt;
      const A = md(x2 + z2), AA = A * A % P, B = md(x2 - z2), BB = B * B % P, E = md(AA - BB);
      const C = md(x3 + z3), D = md(x3 - z3), DA = D * A % P, CB = C * B % P;
      x3 = md((DA + CB) ** 2n);
      z3 = md(x1 * md((DA - CB) ** 2n));
      x2 = AA * BB % P;
      z2 = md(E * md(AA + A24 * E));
    }
    if (cambio) { [x2, x3] = [x3, x2]; [z2, z3] = [z3, z2]; }
    return aBytes(md(x2 * potencia(z2, P - 2n)));
  }
  const BASE = (() => { const b = new Uint8Array(32); b[0] = 9; return b; })();
  const azar = n => crypto.getRandomValues(new Uint8Array(n));
  function parClaves() {
    const privada = azar(32);
    return { privada, publica: x25519(privada, BASE) };
  }

  return { sha256, Sha256, hmac, x25519, parClaves, hex, deHex, azar };
})();
if (typeof module !== "undefined") module.exports = Cripto;
