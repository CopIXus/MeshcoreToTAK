// Decrypt a TAK Portal client key in the browser, then upload the clear PEM.
// Portal keys are PKCS#8 encrypted with pbeWithSHAAnd3-KeyTripleDES-CBC (the
// passphrase is usually atakatak). The ESP32 TLS stack has no 3DES, so the
// working install path has always sent an already-clear key.
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
  function unpad(buf) {
    const n = buf[buf.length - 1];
    if (n < 1 || n > 8) throw new Error('bad padding');
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
  function evpBytesToKey(password, salt) {
    const pw = [];
    for (let i = 0; i < password.length; i++) pw.push(password.charCodeAt(i) & 255);
    let d = new Uint8Array(0), out = new Uint8Array(0);
    while (out.length < 24) {
      const buf = new Uint8Array(d.length + pw.length + salt.length);
      buf.set(d); buf.set(pw, d.length); buf.set(salt, d.length + pw.length);
      d = md5(buf);
      out = concat(out, d);
    }
    return out.slice(0, 24);
  }
  function clearPortalKey(pem, password) {
    if (!pem || pem.indexOf('ENCRYPTED') < 0) return pem;
    const text = pem.replace(/\r/g, '');
    const dek = text.match(/DEK-Info:\s*DES-EDE3-CBC,([0-9A-Fa-f]+)/);
    if (dek) {
      const iv = new Uint8Array((dek[1].match(/../g) || []).map(h => parseInt(h, 16)));
      const key = evpBytesToKey(password || '', iv);
      const plain = unpad(tripleDesCbc(pemBody(text), key, iv, true));
      return pemWrap('RSA PRIVATE KEY', plain);
    }
    const derBytes = pemBody(text);
    const top = derChild(derBytes, 0, derBytes.length);
    const alg = derChild(derBytes, top.start, top.end);
    const oid = derChild(derBytes, alg.start, alg.end);
    const oidHex = Array.from(derBytes.slice(oid.start, oid.end)).map(b => b.toString(16).padStart(2, '0')).join('');
    if (oidHex !== '2a864886f70d010c0103') {
      throw new Error('This key uses an encryption the page cannot open. Run install_certs_to_device.py on a PC.');
    }
    const params = derChild(derBytes, oid.next, alg.end);
    const saltEl = derChild(derBytes, params.start, params.end);
    const iterEl = derChild(derBytes, saltEl.next, params.end);
    const salt = derBytes.slice(saltEl.start, saltEl.end);
    let iter = 0;
    for (let i = iterEl.start; i < iterEl.end; i++) iter = (iter << 8) | derBytes[i];
    const dataEl = derChild(derBytes, alg.next, top.end);
    const key = pkcs12(password || '', salt, iter, 1, 24);
    const iv = pkcs12(password || '', salt, iter, 2, 8);
    const plain = unpad(tripleDesCbc(derBytes.slice(dataEl.start, dataEl.end), key, iv, true));
    return pemWrap('PRIVATE KEY', plain);
  }
  root.clearPortalKey = clearPortalKey;
})(typeof globalThis !== 'undefined' ? globalThis : window);
