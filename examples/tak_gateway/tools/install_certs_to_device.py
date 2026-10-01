#!/usr/bin/env python3
"""Install Portal certs onto Heltec TAK gateway (decrypts atakatak key, POSTs clear PEMs)."""
from pathlib import Path
import json
import sys
import urllib.request
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.serialization import Encoding, PrivateFormat, NoEncryption

def load_bundle(src: Path, password: bytes = b"atakatak"):
    key_path = next(src.glob("*.key"))
    pem_path = next(p for p in src.glob("*.pem") if "trust" not in p.name.lower())
    key = serialization.load_pem_private_key(key_path.read_bytes(), password=password)
    clear_key = key.private_bytes(Encoding.PEM, PrivateFormat.PKCS8, NoEncryption()).decode()
    # Send full Portal .pem (leaf + intermediate + root); device splits client chain vs CA.
    full_pem = pem_path.read_text(encoding="utf-8", errors="ignore")
    return "", full_pem, clear_key


def install(host: str, src: Path, password: str = "atakatak"):
    ca, cert, key = load_bundle(src, password.encode())
    body = json.dumps(
        {"ca": ca, "cert": cert, "key": key, "key_passphrase": password},
        separators=(",", ":"),
    )
    req = urllib.request.Request(
        f"http://{host}/api/certs",
        data=body.encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=30) as resp:
        print(resp.status, resp.read().decode())


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit("usage: install_certs_to_device.py <device-ip> <unzipped-portal-cert-folder> [key-passphrase]")
    install(sys.argv[1], Path(sys.argv[2]), sys.argv[3] if len(sys.argv) > 3 else "atakatak")
