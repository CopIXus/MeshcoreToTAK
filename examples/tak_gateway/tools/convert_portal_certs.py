#!/usr/bin/env python3
"""Decrypt Portal Download Certs into clear PEMs for the Heltec TAK gateway."""
from pathlib import Path
from cryptography.hazmat.primitives.serialization import pkcs12, Encoding, PrivateFormat, NoEncryption
from cryptography.hazmat.primitives import serialization
from cryptography import x509
import sys

def convert(src: Path, out: Path, password: bytes = b"atakatak") -> None:
    out.mkdir(parents=True, exist_ok=True)
    key_path = next(src.glob("*.key"))
    pem_path = next(p for p in src.glob("*.pem") if "trust" not in p.name.lower())
    key = serialization.load_pem_private_key(key_path.read_bytes(), password=password)
    clear = key.private_bytes(Encoding.PEM, PrivateFormat.TraditionalOpenSSL, NoEncryption())
    (out / "client.key").write_bytes(clear)

    pem = pem_path.read_bytes()
    certs = []
    start = 0
    while True:
        i = pem.find(b"-----BEGIN CERTIFICATE-----", start)
        if i < 0:
            break
        j = pem.find(b"-----END CERTIFICATE-----", i)
        block = pem[i : j + len(b"-----END CERTIFICATE-----")]
        certs.append(x509.load_pem_x509_certificate(block))
        start = j + 1
    (out / "client.pem").write_bytes(certs[0].public_bytes(Encoding.PEM))
    ca = b"".join(c.public_bytes(Encoding.PEM) for c in certs[1:])
    # also try truststore p12
    for p12 in src.glob("*truststore*.p12"):
        _, cert, add = pkcs12.load_key_and_certificates(p12.read_bytes(), password)
        bag = ([] if cert is None else [cert]) + list(add or [])
        for c in bag:
            der = c.public_bytes(Encoding.DER)
            if der not in {x509.load_pem_x509_certificate(b).public_bytes(Encoding.DER) for b in ca.split(b"-----BEGIN") if b.strip()}:
                ca += c.public_bytes(Encoding.PEM)
    (out / "ca.pem").write_bytes(ca)
    print("wrote", out)


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit("usage: convert_portal_certs.py <unzipped-portal-cert-folder> <output-folder>")
    convert(Path(sys.argv[1]), Path(sys.argv[2]))
