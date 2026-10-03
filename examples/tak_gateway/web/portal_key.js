// Decrypt a TAK Portal client key in the browser, then upload the clear PEM.
// Older Portal keys are PKCS#8 pbeWithSHAAnd3-KeyTripleDES-CBC. Newer ones are
// PBES2 (PBKDF2 + AES-256-CBC) or a traditional OpenSSL AES PEM. One passphrase
// unlocks both the write and the read key. The ESP32 TLS stack cannot decrypt
// these, so the page always sends an already-clear key.
(function (root) {
  function rotl(x, n) { return ((x << n) | (x >>> (32 - n))) >>> 0; }
  function sha1(bytes) {
    const n = bytes.length, words = Math.ceil((n + 9) / 64) * 16;
    const w = new Uint32Array(words);
    for (let i = 0; i < n; i++) w[i >> 2] |= bytes[i] << (24 - (i & 3) * 8);
    w[n >> 2] |= 0x80 << (24 - (n & 3) * 8);
    const bits = n * 8;
    w[words - 2] = Math.floor(bits / 0x100000000);
    w[words - 1] = bits >>> 0;
    let a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476, e = 0xc3d2e1f0;
    const m = new Uint32Array(80);
    for (let i = 0; i < words; i += 16) {
      for (let t = 0; t < 16; t++) m[t] = w[i + t];
      for (let t = 16; t < 80; t++) m[t] = rotl(m[t - 3] ^ m[t - 8] ^ m[t - 14] ^ m[t - 16], 1);
      let A = a, B = b, C = c, D = d, E = e;
      for (let t = 0; t < 80; t++) {
        let f, k;
        if (t < 20) { f = (B & C) | (~B & D); k = 0x5a827999; }
        else if (t < 40) { f = B ^ C ^ D; k = 0x6ed9eba1; }
        else if (t < 60) { f = (B & C) | (B & D) | (C & D); k = 0x8f1bbcdc; }
        else { f = B ^ C ^ D; k = 0xca62c1d6; }
        const tmp = (rotl(A, 5) + f + E + k + m[t]) >>> 0;
        E = D; D = C; C = rotl(B, 30); B = A; A = tmp;
      }
      a = (a + A) >>> 0; b = (b + B) >>> 0; c = (c + C) >>> 0; d = (d + D) >>> 0; e = (e + E) >>> 0;
    }
    const out = new Uint8Array(20);
    [a, b, c, d, e].forEach((x, i) => { out[i * 4] = x >>> 24; out[i * 4 + 1] = x >>> 16; out[i * 4 + 2] = x >>> 8; out[i * 4 + 3] = x; });
    return out;
  }
  function md5(bytes) {
    function add(x, y) { return (x + y) >>> 0; }
    function rl(x, n) { return (x << n) | (x >>> (32 - n)); }
    const n = bytes.length, blocks = (((n + 8) >>> 6) + 1) * 16;
    const w = new Uint32Array(blocks);
    for (let i = 0; i < n; i++) w[i >> 2] |= bytes[i] << ((i & 3) * 8);
    w[n >> 2] |= 0x80 << ((n & 3) * 8);
    const bits = n * 8;
    w[blocks - 2] = bits >>> 0;
    w[blocks - 1] = Math.floor(bits / 0x100000000);
    let a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;
    const K = new Uint32Array(64), S = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
    for (let i = 0; i < 64; i++) K[i] = Math.floor(Math.abs(Math.sin(i + 1)) * 0x100000000) >>> 0;
    for (let i = 0; i < blocks; i += 16) {
      let A = a, B = b, C = c, D = d;
      for (let t = 0; t < 64; t++) {
        let f, g;
        if (t < 16) { f = (B & C) | (~B & D); g = t; }
        else if (t < 32) { f = (D & B) | (~D & C); g = (5 * t + 1) & 15; }
        else if (t < 48) { f = B ^ C ^ D; g = (3 * t + 5) & 15; }
        else { f = C ^ (B | ~D); g = (7 * t) & 15; }
        const tmp = D;
        D = C; C = B;
        B = add(B, rl(add(add(A, f), add(K[t], w[i + g])), S[(t >> 4) * 4 + (t & 3)]));
        A = tmp;
      }
      a = add(a, A); b = add(b, B); c = add(c, C); d = add(d, D);
    }
    const out = new Uint8Array(16);
    [a, b, c, d].forEach((x, i) => { out[i * 4] = x; out[i * 4 + 1] = x >>> 8; out[i * 4 + 2] = x >>> 16; out[i * 4 + 3] = x >>> 24; });
    return out;
  }

  const IP = [58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4, 62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8, 57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3, 61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7];
  const FP = [40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31, 38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29, 36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27, 34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25];
  const E = [32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9, 8, 9, 10, 11, 12, 13, 12, 13, 14, 15, 16, 17, 16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1];
  const PC1 = [57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18, 10, 2, 59, 51, 43, 35, 27, 19, 11, 3, 60, 52, 44, 36, 63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22, 14, 6, 61, 53, 45, 37, 29, 21, 13, 5, 28, 20, 12, 4];
  const PC2 = [14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10, 23, 19, 12, 4, 26, 8, 16, 7, 27, 20, 13, 2, 41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32];
  const SH = [1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1];
  const SB = [
    [14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7, 0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8, 4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0, 15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13],
    [15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10, 3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5, 0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15, 13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9],
    [10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8, 13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1, 13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7, 1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12],
    [7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15, 13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9, 10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4, 3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14],
    [2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9, 14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6, 4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14, 11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3],
    [12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11, 10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8, 9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6, 4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13],
    [4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1, 13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6, 1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2, 6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12],
    [13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7, 1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2, 7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8, 2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11]
  ];
  function bit(src, pos) { const i = pos - 1; return (src[i >> 3] >> (7 - (i & 7))) & 1; }
  function perm(src, table) {
    const out = new Uint8Array(Math.ceil(table.length / 8));
    table.forEach((p, i) => { if (bit(src, p)) out[i >> 3] |= 1 << (7 - (i & 7)); });
    return out;
  }
  function rol28(v, n) { return ((v << n) | (v >>> (28 - n))) & 0x0fffffff; }
  function subkeys(key8, decrypt) {
    const c0 = perm(key8, PC1);
    let c = 0, d = 0;
    for (let i = 0; i < 28; i++) {
      if (bit(c0, i + 1)) c |= 1 << (27 - i);
      if (bit(c0, i + 29)) d |= 1 << (27 - i);
    }
    const ks = [];
    for (let r = 0; r < 16; r++) {
      c = rol28(c, SH[r]); d = rol28(d, SH[r]);
      const cd = new Uint8Array(7);
      for (let i = 0; i < 28; i++) if ((c >> (27 - i)) & 1) cd[i >> 3] |= 1 << (7 - (i & 7));
      for (let i = 0; i < 28; i++) if ((d >> (27 - i)) & 1) cd[(28 + i) >> 3] |= 1 << (7 - ((28 + i) & 7));
      ks.push(perm(cd, PC2));
    }
    return decrypt ? ks.reverse() : ks;
  }
  function desBlock(block, ks) {
    const ip = perm(block, IP);
    let l = ip.slice(0, 4), r = ip.slice(4, 8);
    for (let round = 0; round < 16; round++) {
      const er = perm(r, E);
      for (let i = 0; i < 6; i++) er[i] ^= ks[round][i];
      const s = new Uint8Array(4);
      for (let i = 0; i < 8; i++) {
        const bit6 = (er[Math.floor((i * 6) / 8)] << 8 | er[Math.floor((i * 6) / 8) + 1] || 0) >> (10 - ((i * 6) & 7));
        const v = bit6 & 63;
        const row = ((v >> 5) << 1) | (v & 1), col = (v >> 1) & 15;
        const nib = SB[i][row * 16 + col];
        s[i >> 1] |= nib << (i & 1 ? 0 : 4);
      }
      const f = perm(s, [16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10, 2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25]);
      const nl = r;
      r = l.map((b, i) => b ^ f[i]);
      l = nl;
    }
    const pre = new Uint8Array(8);
    pre.set(r, 0); pre.set(l, 4);
    return perm(pre, FP);
  }
  function desCbc(data, key8, iv, decrypt) {
    const ks = subkeys(key8, decrypt);
    const out = new Uint8Array(data.length);
    let prev = iv;
    for (let i = 0; i < data.length; i += 8) {
      const block = data.slice(i, i + 8);
      if (decrypt) {
        const p = desBlock(block, ks);
        for (let j = 0; j < 8; j++) out[i + j] = p[j] ^ prev[j];
        prev = block;
      } else {
        const x = block.map((b, j) => b ^ prev[j]);
        const c = desBlock(x, ks);
        out.set(c, i);
        prev = c;
      }
    }
    return out;
  }
  function tripleDesCbc(data, key, iv, decrypt) {
    const k1 = key.slice(0, 8), k2 = key.slice(8, 16), k3 = key.length >= 24 ? key.slice(16, 24) : k1;
    if (!decrypt) return desCbc(desCbc(desCbc(data, k1, iv, false), k2, zero8(), true), k3, zero8(), false);
    // Decrypt is D(k3) then E(k2) then D(k1), with CBC applied on the outside.
    const ks3 = subkeys(k3, true), ks2 = subkeys(k2, false), ks1 = subkeys(k1, true);
    const out = new Uint8Array(data.length);
    let prev = iv;
    for (let i = 0; i < data.length; i += 8) {
      const block = data.slice(i, i + 8);
      let p = desBlock(block, ks3);
      p = desBlock(p, ks2);
      p = desBlock(p, ks1);
      for (let j = 0; j < 8; j++) out[i + j] = p[j] ^ prev[j];
      prev = block;
    }
    return out;
  }
  function zero8() { return new Uint8Array(8); }
  function unpad(buf, block) {
    const n = buf[buf.length - 1];
    if (n < 1 || n > (block || 8)) throw new Error('bad padding');
    for (let i = 0; i < n; i++) if (buf[buf.length - 1 - i] !== n) throw new Error('bad padding');
    return buf.slice(0, buf.length - n);
  }
  function concat(a, b) { const o = new Uint8Array(a.length + b.length); o.set(a); o.set(b, a.length); return o; }
  function fillV(src, v) {
    const o = new Uint8Array(v);
    for (let i = 0; i < v; i++) o[i] = src[i % src.length];
    return o;
  }
  function addB(I, B) {
    const v = B.length, o = new Uint8Array(I.length);
    for (let off = 0; off < I.length; off += v) {
      let carry = 1;
      for (let i = v - 1; i >= 0; i--) {
        const s = I[off + i] + B[i] + carry;
        o[off + i] = s & 255;
        carry = s >> 8;
      }
    }
    return o;
  }
  // PKCS#12 password-based key derivation (RFC 7292 appendix B), SHA-1.
  function pkcs12(password, salt, iterations, id, bytes) {
    const u = 20, v = 64;
    const pw = [];
    for (let i = 0; i < password.length; i++) { pw.push(0, password.charCodeAt(i) & 255); }
    pw.push(0, 0);
    const D = new Uint8Array(v).fill(id);
    let I = fillV(salt, Math.ceil(salt.length / v) * v);
    if (pw.length) I = concat(I, fillV(new Uint8Array(pw), Math.ceil(pw.length / v) * v));
    const out = [];
    const c = Math.ceil(bytes / u);
    for (let i = 0; i < c; i++) {
      let A = sha1(concat(D, I));
      for (let j = 1; j < iterations; j++) A = sha1(A);
      for (let j = 0; j < A.length; j++) out.push(A[j]);
      const B = fillV(A, v);
      I = addB(I, B);
    }
    return new Uint8Array(out.slice(0, bytes));
  }
  function b64dec(s) {
    const bin = atob(s.replace(/\s/g, ''));
    const o = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) o[i] = bin.charCodeAt(i);
    return o;
  }
  function b64enc(bytes) {
    let s = '';
    for (let i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
    return btoa(s).replace(/(.{64})/g, '$1\n');
  }
  function derLen(buf, i) {
    let n = buf[i++], len = n;
    if (n & 0x80) {
      len = 0;
      const c = n & 0x7f;
      for (let k = 0; k < c; k++) len = (len << 8) | buf[i++];
    }
    return { i, len };
  }
  function derChild(buf, start, end) {
    const tag = buf[start];
    const L = derLen(buf, start + 1);
    return { tag, start: L.i, end: L.i + L.len, next: L.i + L.len };
  }
  function pemBody(pem) {
    const lines = pem.replace(/\r/g, '').split('\n').filter(l => l && !l.startsWith('-----') && !l.startsWith('Proc-Type') && !l.startsWith('DEK-Info'));
    return b64dec(lines.join(''));
  }
  function pemWrap(label, der) {
    return '-----BEGIN ' + label + '-----\n' + b64enc(der).replace(/\n$/, '') + '\n-----END ' + label + '-----\n';
  }
  function hexBytes(s) {
    const o = new Uint8Array(s.length / 2);
    for (let i = 0; i < o.length; i++) o[i] = parseInt(s.substr(i * 2, 2), 16);
    return o;
  }
  function hexOf(bytes) {
    let s = '';
    for (let i = 0; i < bytes.length; i++) s += bytes[i].toString(16).padStart(2, '0');
    return s;
  }
  function readInt(buf, el) {
    let v = 0;
    for (let i = el.start; i < el.end; i++) v = (v * 256) + buf[i];
    return v;
  }
  // OpenSSL EVP_BytesToKey, MD5, one iteration. The salt is the first 8 bytes of the IV.
  function evpBytesToKey(password, iv, n) {
    const salt = iv.slice(0, 8);
    const pw = [];
    for (let i = 0; i < password.length; i++) pw.push(password.charCodeAt(i) & 255);
    let d = new Uint8Array(0), out = new Uint8Array(0);
    while (out.length < n) {
      const buf = new Uint8Array(d.length + pw.length + salt.length);
      buf.set(d); buf.set(pw, d.length); buf.set(salt, d.length + pw.length);
      d = md5(buf);
      out = concat(out, d);
    }
    return out.slice(0, n);
  }
  function keyBlock(pem) {
    const text = pem.replace(/\r/g, '');
    const enc = text.match(/-----BEGIN ENCRYPTED PRIVATE KEY-----[\s\S]*?-----END ENCRYPTED PRIVATE KEY-----/);
    if (enc) return enc[0];
    const rsa = text.match(/-----BEGIN RSA PRIVATE KEY-----[\s\S]*?-----END RSA PRIVATE KEY-----/);
    if (rsa) return rsa[0];
    const pk = text.match(/-----BEGIN PRIVATE KEY-----[\s\S]*?-----END PRIVATE KEY-----/);
    if (pk) return pk[0];
    return text;
  }
  function unsupported() {
    throw new Error('This key uses an encryption the page cannot open. Run install_certs_to_device.py on a PC.');
  }
  // Browsers only expose crypto.subtle on HTTPS or localhost, and this page is served over
  // plain HTTP, so SHA-256, PBKDF2 and AES are done here.
  const K256 = new Uint32Array([
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2]);
  function sha256(bytes) {
    const n = bytes.length, words = Math.ceil((n + 9) / 64) * 16;
    const w = new Uint32Array(words);
    for (let i = 0; i < n; i++) w[i >> 2] |= bytes[i] << (24 - (i & 3) * 8);
    w[n >> 2] |= 0x80 << (24 - (n & 3) * 8);
    w[words - 2] = Math.floor(n * 8 / 0x100000000);
    w[words - 1] = (n * 8) >>> 0;
    const h = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
    const m = new Uint32Array(64);
    const rotr = (x, k) => (x >>> k) | (x << (32 - k));
    for (let i = 0; i < words; i += 16) {
      for (let t = 0; t < 16; t++) m[t] = w[i + t];
      for (let t = 16; t < 64; t++) {
        const s0 = rotr(m[t - 15], 7) ^ rotr(m[t - 15], 18) ^ (m[t - 15] >>> 3);
        const s1 = rotr(m[t - 2], 17) ^ rotr(m[t - 2], 19) ^ (m[t - 2] >>> 10);
        m[t] = (m[t - 16] + s0 + m[t - 7] + s1) >>> 0;
      }
      let [a, b, c, d, e, f, g, k] = h;
      for (let t = 0; t < 64; t++) {
        const t1 = (k + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K256[t] + m[t]) >>> 0;
        const t2 = ((rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c))) >>> 0;
        k = g; g = f; f = e; e = (d + t1) >>> 0; d = c; c = b; b = a; a = (t1 + t2) >>> 0;
      }
      h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
    }
    const out = new Uint8Array(32);
    h.forEach((x, i) => { out[i * 4] = x >>> 24; out[i * 4 + 1] = x >>> 16; out[i * 4 + 2] = x >>> 8; out[i * 4 + 3] = x; });
    return out;
  }
  const HASHES = { 'SHA-1': sha1, 'SHA-256': sha256 };
  function hmac(hash, key, msg) {
    if (key.length > 64) key = hash(key);
    const k = new Uint8Array(64);
    k.set(key);
    return hash(concat(k.map(b => b ^ 0x5c), hash(concat(k.map(b => b ^ 0x36), msg))));
  }
  async function pbkdf2(password, salt, iterations, hash, bytes) {
    const pw = new TextEncoder().encode(password);
    const fn = HASHES[hash];
    if (!fn) {
      const subtle = root.crypto && root.crypto.subtle;
      if (!subtle) throw new Error('This key needs ' + hash + ', which the page can only use over HTTPS. Run install_certs_to_device.py on a PC.');
      const mat = await subtle.importKey('raw', pw, 'PBKDF2', false, ['deriveBits']);
      return new Uint8Array(await subtle.deriveBits({ name: 'PBKDF2', salt, iterations, hash }, mat, bytes * 8));
    }
    const out = new Uint8Array(bytes);
    for (let blk = 1, off = 0; off < bytes; blk++) {
      let u = hmac(fn, pw, concat(salt, Uint8Array.of(blk >>> 24, (blk >>> 16) & 255, (blk >>> 8) & 255, blk & 255)));
      const t = u.slice();
      for (let i = 1; i < iterations; i++) {
        u = hmac(fn, pw, u);
        for (let j = 0; j < t.length; j++) t[j] ^= u[j];
      }
      out.set(t.slice(0, Math.min(t.length, bytes - off)), off);
      off += t.length;
    }
    return out;
  }
  const AES_S = new Uint8Array(256), AES_SI = new Uint8Array(256);
  (function () {
    const rotl8 = (x, k) => ((x << k) | (x >>> (8 - k))) & 255;
    let p = 1, q = 1;
    do {
      p = (p ^ (p << 1) ^ (p & 0x80 ? 0x1b : 0)) & 255;
      q ^= q << 1; q ^= q << 2; q ^= q << 4; q &= 255;
      if (q & 0x80) q ^= 0x09;
      AES_S[p] = q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63;
    } while (p !== 1);
    AES_S[0] = 0x63;
    for (let i = 0; i < 256; i++) AES_SI[AES_S[i]] = i;
  })();
  function gmul(a, b) {
    let r = 0;
    while (b) {
      if (b & 1) r ^= a;
      a = ((a << 1) ^ (a & 0x80 ? 0x1b : 0)) & 255;
      b >>= 1;
    }
    return r;
  }
  function aesExpand(key) {
    const nk = key.length / 4, nr = nk + 6, w = new Uint8Array(16 * (nr + 1));
    w.set(key);
    let rcon = 1;
    for (let i = nk; i < 4 * (nr + 1); i++) {
      let t = w.slice((i - 1) * 4, i * 4);
      if (i % nk === 0) {
        t = Uint8Array.of(AES_S[t[1]] ^ rcon, AES_S[t[2]], AES_S[t[3]], AES_S[t[0]]);
        rcon = gmul(rcon, 2);
      } else if (nk > 6 && i % nk === 4) {
        t = t.map(b => AES_S[b]);
      }
      for (let j = 0; j < 4; j++) w[i * 4 + j] = w[(i - nk) * 4 + j] ^ t[j];
    }
    return { w, nr };
  }
  function aesDecryptBlock(inp, ks) {
    const { w, nr } = ks, s = new Uint8Array(16), t = new Uint8Array(16);
    for (let i = 0; i < 16; i++) s[i] = inp[i] ^ w[nr * 16 + i];
    for (let round = nr - 1; round >= 0; round--) {
      for (let c = 0; c < 4; c++) {
        for (let r = 0; r < 4; r++) t[r + 4 * c] = AES_SI[s[r + 4 * ((c - r + 4) % 4)]] ^ w[round * 16 + r + 4 * c];
      }
      if (!round) return t;
      for (let c = 0; c < 4; c++) {
        const a0 = t[4 * c], a1 = t[4 * c + 1], a2 = t[4 * c + 2], a3 = t[4 * c + 3];
        s[4 * c] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
        s[4 * c + 1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
        s[4 * c + 2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
        s[4 * c + 3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
      }
    }
    return t;
  }
  async function aesCbcDecrypt(keyBytes, iv, data) {
    if (data.length % 16) throw new Error('bad ciphertext length');
    const ks = aesExpand(keyBytes), out = new Uint8Array(data.length);
    let prev = iv;
    for (let i = 0; i < data.length; i += 16) {
      const block = data.slice(i, i + 16), p = aesDecryptBlock(block, ks);
      for (let j = 0; j < 16; j++) out[i + j] = p[j] ^ prev[j];
      prev = block;
    }
    return unpad(out, 16);
  }
  function cipherInfo(oidHex) {
    if (oidHex === '60864801650304012a') return { aes: 32 };
    if (oidHex === '608648016503040116') return { aes: 24 };
    if (oidHex === '608648016503040102') return { aes: 16 };
    if (oidHex === '2a864886f70d0307') return { des: 24 };
    return null;
  }
  function prfHash(oidHex) {
    if (oidHex === '2a864886f70d0207') return 'SHA-1';
    if (oidHex === '2a864886f70d0209') return 'SHA-256';
    if (oidHex === '2a864886f70d020a') return 'SHA-384';
    if (oidHex === '2a864886f70d020b') return 'SHA-512';
    return '';
  }
  async function decryptPbes2(derBytes, top, alg, oid, password) {
    const params = derChild(derBytes, oid.next, alg.end);
    const kdf = derChild(derBytes, params.start, params.end);
    const kdfOid = derChild(derBytes, kdf.start, kdf.end);
    if (hexOf(derBytes.slice(kdfOid.start, kdfOid.end)) !== '2a864886f70d01050c') unsupported();
    const kdfParams = derChild(derBytes, kdfOid.next, kdf.end);
    const saltEl = derChild(derBytes, kdfParams.start, kdfParams.end);
    let p = saltEl.next;
    const iterEl = derChild(derBytes, p, kdfParams.end);
    p = iterEl.next;
    let keyLen = 0, hash = 'SHA-1';
    while (p < kdfParams.end) {
      const el = derChild(derBytes, p, kdfParams.end);
      p = el.next;
      if (el.tag === 0x02) keyLen = readInt(derBytes, el);
      else if (el.tag === 0x30) {
        const prfOid = derChild(derBytes, el.start, el.end);
        hash = prfHash(hexOf(derBytes.slice(prfOid.start, prfOid.end)));
        if (!hash) unsupported();
      }
    }
    const enc = derChild(derBytes, kdf.next, params.end);
    const encOid = derChild(derBytes, enc.start, enc.end);
    const info = cipherInfo(hexOf(derBytes.slice(encOid.start, encOid.end)));
    if (!info) unsupported();
    const ivEl = derChild(derBytes, encOid.next, enc.end);
    const iv = derBytes.slice(ivEl.start, ivEl.end);
    const dataEl = derChild(derBytes, alg.next, top.end);
    const data = derBytes.slice(dataEl.start, dataEl.end);
    const n = keyLen || info.aes || info.des;
    const key = await pbkdf2(password || '', derBytes.slice(saltEl.start, saltEl.end), readInt(derBytes, iterEl), hash, n);
    if (info.aes) return pemWrap('PRIVATE KEY', await aesCbcDecrypt(key, iv, data));
    return pemWrap('PRIVATE KEY', unpad(tripleDesCbc(data, key, iv, true)));
  }
  async function clearPortalKey(pem, password) {
    if (!pem || pem.indexOf('ENCRYPTED') < 0) return pem;
    const text = keyBlock(pem);
    const dek = text.match(/DEK-Info:\s*([A-Za-z0-9-]+),([0-9A-Fa-f]+)/);
    if (dek) {
      const iv = hexBytes(dek[2]);
      const body = pemBody(text);
      const cipher = dek[1].toUpperCase();
      if (cipher === 'DES-EDE3-CBC') {
        return pemWrap('RSA PRIVATE KEY', unpad(tripleDesCbc(body, evpBytesToKey(password || '', iv, 24), iv, true)));
      }
      const aesLen = cipher === 'AES-256-CBC' ? 32 : cipher === 'AES-192-CBC' ? 24 : cipher === 'AES-128-CBC' ? 16 : 0;
      if (!aesLen) unsupported();
      return pemWrap('RSA PRIVATE KEY', await aesCbcDecrypt(evpBytesToKey(password || '', iv, aesLen), iv, body));
    }
    const derBytes = pemBody(text);
    const top = derChild(derBytes, 0, derBytes.length);
    const alg = derChild(derBytes, top.start, top.end);
    const oid = derChild(derBytes, alg.start, alg.end);
    const oidHex = hexOf(derBytes.slice(oid.start, oid.end));
    if (oidHex === '2a864886f70d01050d') return decryptPbes2(derBytes, top, alg, oid, password);
    // pbeWithSHAAnd3-KeyTripleDES-CBC and the 2-key variant.
    const desLen = oidHex === '2a864886f70d010c0103' ? 24 : oidHex === '2a864886f70d010c0104' ? 16 : 0;
    if (!desLen) unsupported();
    const params = derChild(derBytes, oid.next, alg.end);
    const saltEl = derChild(derBytes, params.start, params.end);
    const iterEl = derChild(derBytes, saltEl.next, params.end);
    const salt = derBytes.slice(saltEl.start, saltEl.end);
    const iter = readInt(derBytes, iterEl);
    const dataEl = derChild(derBytes, alg.next, top.end);
    const key = pkcs12(password || '', salt, iter, 1, desLen);
    const iv = pkcs12(password || '', salt, iter, 2, 8);
    return pemWrap('PRIVATE KEY', unpad(tripleDesCbc(derBytes.slice(dataEl.start, dataEl.end), key, iv, true)));
  }
  root.clearPortalKey = clearPortalKey;
})(typeof globalThis !== 'undefined' ? globalThis : window);
