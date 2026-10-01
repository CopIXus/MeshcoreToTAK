#!/usr/bin/env python3
"""Build the TAK gateway setup page assets into a C header.

- Crops a curated set of CloudTAK default (2525B) icons out of CloudTAK's
  api/icons/generator.png sprite into a small palette PNG.
- Injects the icon index into web/index.html, gzips it.
- Writes examples/tak_gateway/tak/TakWebAssets.h (PROGMEM byte arrays).

Run after editing web/index.html:
    python examples/tak_gateway/tools/build_web_assets.py
"""
import gzip
import io
import json
import pathlib
import urllib.request

from PIL import Image

GW = pathlib.Path(__file__).resolve().parent.parent
ICON_DIR = GW / "tools" / "cloudtak_icons"  # download cache, not committed
HTML_SRC = GW / "web" / "index.html"
OUT_H = GW / "tak" / "TakWebAssets.h"
CLOUDTAK_RAW = "https://raw.githubusercontent.com/dfpc-coe/CloudTAK/main/api/icons/"

AFFILIATIONS = "fhnu"  # friendly, hostile, neutral, unknown -> sprite columns

# (2525B function suffix after "a-<aff>-", label, group)
FUNCTIONS = [
    ("G-U-U-S-R", "Radio unit", "Comms"),
    ("G-U-U-S-R-W", "Relay", "Comms"),
    ("G-U-U-S", "Signal unit", "Comms"),
    ("G-E-S", "Sensor", "Comms"),
    ("G-U-U-M-R-X", "Ground station", "Comms"),
    ("G-I-U-T", "Telecom facility", "Comms"),
    ("G", "Ground track", "Ground"),
    ("G-U", "Unit", "Ground"),
    ("G-U-C", "Combat", "Ground"),
    ("G-U-C-I", "Infantry", "Ground"),
    ("G-U-C-R", "Recon", "Ground"),
    ("G-U-H", "Headquarters", "Ground"),
    ("G-U-C-V-S", "Search & rescue", "Public safety"),
    ("G-U-S-M", "Medical", "Public safety"),
    ("G-I-X-H", "Hospital", "Public safety"),
    ("G-E-V-U-A", "Ambulance", "Public safety"),
    ("G-U-U-L", "Law enforcement", "Public safety"),
    ("G-U-U-L-C", "Civil police", "Public safety"),
    ("G-E-V", "Ground vehicle", "Vehicles"),
    ("G-E-V-C", "Civilian vehicle", "Vehicles"),
    ("G-U-S-T", "Transportation", "Vehicles"),
    ("G-I", "Installation", "Facilities"),
    ("G-I-U-E", "Electric power", "Facilities"),
    ("G-I-U-P", "Water services", "Facilities"),
    ("A", "Air track", "Air & sea"),
    ("A-C", "Civil aircraft", "Air & sea"),
    ("A-M-H", "Rotary wing", "Air & sea"),
    ("A-M-F-Q", "Drone", "Air & sea"),
    ("G-U-C-V-U", "UAV unit", "Air & sea"),
    ("S", "Sea surface", "Air & sea"),
    ("S-X", "Civil vessel", "Air & sea"),
]

# Non-2525 markers from CloudTAK's custom set (last sprite row)
MARKERS = [
    ("b-m-p-s-p-i", "SPI"),
    ("b-r-f-h-c", "CASEVAC"),
]

SZ = 32

# CloudTAK-Data "Default" iconset: bundled with ATAK, CloudTAK and TAK Portal, so a
# usericon path into it renders the same on all three. (group, file stem, label)
ICONSET_ZIP = "https://raw.githubusercontent.com/dfpc-coe/CloudTAK-Data/v1.1.0/iconsets/Default.zip"
ICONSET_ITEMS = [
    ("Hiking", "star", "Star"), ("Hiking", "hiker", "Hiker"), ("Hiking", "camp", "Camp"),
    ("Hiking", "tent", "Tent"), ("Hiking", "cabin", "Cabin"), ("Hiking", "summit", "Summit"),
    ("Hiking", "mountain", "Mountain"), ("Hiking", "compass", "Compass"), ("Hiking", "map", "Map"),
    ("Hiking", "backpack", "Backpack"), ("Hiking", "potablewater", "Water"), ("Hiking", "climbing", "Climbing"),
    ("People", "Man", "Man"), ("People", "Woman", "Woman"), ("People", "walk", "Walking"),
    ("People", "Police", "Police"), ("People", "Fireman", "Firefighter"), ("People", "Bicyclist", "Cyclist"),
    ("People", "construction", "Worker"), ("People", "suspect", "Suspect"), ("People", "Handicap", "Assist"),
    ("Buildings", "house", "House"), ("Buildings", "hospital", "Hospital"), ("Buildings", "firehouse", "Fire station"),
    ("Buildings", "government", "Government"), ("Buildings", "tower", "Tower"), ("Buildings", "office", "Office"),
    ("Buildings", "shops", "Shops"), ("Buildings", "farm", "Farm"), ("Buildings", "lighthouse", "Lighthouse"),
    ("Buildings", "tentlarge", "Shelter"),
    ("Transportation", "Car", "Car"), ("Transportation", "PickupTruck", "Pickup"),
    ("Transportation", "Ambulance", "Ambulance"), ("Transportation", "FireTruck", "Fire truck"),
    ("Transportation", "Helicopter", "Helicopter"), ("Transportation", "Plane", "Plane"),
    ("Transportation", "Boat", "Boat"), ("Transportation", "Motorcycle", "Motorcycle"),
    ("Transportation", "ATV", "ATV"), ("Transportation", "Bus", "Bus"), ("Transportation", "Bicycle", "Bicycle"),
    ("Transportation", "caution", "Caution"), ("Transportation", "accident", "Accident"),
    ("Transportation", "Fuel", "Fuel"),
    ("Military", "radar", "Radar"), ("Military", "medical", "Medical"), ("Military", "soldier", "Soldier"),
    ("Military", "Humvee", "Humvee"), ("Military", "convoy", "Convoy"), ("Military", "K9", "K9"),
    ("Hunting", "crosshair", "Crosshair"), ("Hunting", "target", "Target"),
    ("Weather", "sun", "Sun"), ("Weather", "Rain", "Rain"), ("Weather", "Snow", "Snow"),
    ("Animals", "pawprint", "Pawprint"), ("Animals", "horse", "Horse"),
]
ICONSET_COLS = 8


def fetch(name):
    p = ICON_DIR / name
    if not p.exists():
        ICON_DIR.mkdir(parents=True, exist_ok=True)
        print("downloading", name)
        urllib.request.urlretrieve(CLOUDTAK_RAW + name, p)
    return p


def build_sprite():
    index = json.loads(fetch("generator.json").read_text())
    src = Image.open(fetch("generator.png")).convert("RGBA")
    rows = len(FUNCTIONS) + 1
    sprite = Image.new("RGBA", (SZ * len(AFFILIATIONS), SZ * rows), (0, 0, 0, 0))

    def put(key, col, row):
        e = index[key]
        tile = src.crop((e["x"], e["y"], e["x"] + e["width"], e["y"] + e["height"]))
        if tile.size != (SZ, SZ):
            tile = tile.resize((SZ, SZ), Image.LANCZOS)
        sprite.paste(tile, (col * SZ, row * SZ))

    for r, (fn, _, _) in enumerate(FUNCTIONS):
        for c, aff in enumerate(AFFILIATIONS):
            put(f"a-{aff}-{fn}", c, r)
    for c, (key, _) in enumerate(MARKERS):
        put(key, c, len(FUNCTIONS))

    q = sprite.quantize(colors=128, method=Image.Quantize.FASTOCTREE)
    buf = io.BytesIO()
    q.save(buf, "PNG", optimize=True)
    return buf.getvalue()


def build_iconset_sprite():
    import zipfile
    import xml.etree.ElementTree as ET

    zpath = ICON_DIR / "iconsets" / "Default.zip"
    if not zpath.exists():
        zpath.parent.mkdir(parents=True, exist_ok=True)
        print("downloading Default.zip")
        urllib.request.urlretrieve(ICONSET_ZIP, zpath)
    z = zipfile.ZipFile(zpath)
    names = z.namelist()
    xml_name = next(n for n in names if n.endswith("iconset.xml"))
    uid = ET.fromstring(z.read(xml_name)).get("uid")
    prefix = xml_name[: -len("iconset.xml")]
    by_lower = {n[len(prefix):].lower(): n for n in names if n.startswith(prefix)}

    rows = (len(ICONSET_ITEMS) + ICONSET_COLS - 1) // ICONSET_COLS
    sprite = Image.new("RGBA", (SZ * ICONSET_COLS, SZ * rows), (0, 0, 0, 0))
    items = []
    for i, (group, stem, label) in enumerate(ICONSET_ITEMS):
        member = by_lower.get(f"{group}/{stem}.png".lower())
        if not member:
            raise SystemExit(f"missing in Default iconset: {group}/{stem}.png")
        rel = member[len(prefix):]  # exact case, as clients match it
        tile = Image.open(io.BytesIO(z.read(member))).convert("RGBA")
        tile.thumbnail((SZ, SZ), Image.LANCZOS)
        x = (i % ICONSET_COLS) * SZ + (SZ - tile.width) // 2
        y = (i // ICONSET_COLS) * SZ + (SZ - tile.height) // 2
        sprite.paste(tile, (x, y), tile)
        items.append([rel, label])

    q = sprite.quantize(colors=256, method=Image.Quantize.FASTOCTREE)
    buf = io.BytesIO()
    q.save(buf, "PNG", optimize=True)
    return uid, items, buf.getvalue()


def c_array(name, data):
    lines = [f"static const uint8_t {name}[] PROGMEM = {{"]
    for i in range(0, len(data), 20):
        lines.append("  " + ",".join(f"0x{b:02x}" for b in data[i:i + 20]) + ",")
    lines.append("};")
    lines.append(f"static const size_t {name}_LEN = {len(data)};")
    return "\n".join(lines)


def main():
    png = build_sprite()
    set_uid, set_items, set_png = build_iconset_sprite()
    icon_index = {
        "aff": AFFILIATIONS,
        "fn": [[fn, label, grp] for fn, label, grp in FUNCTIONS],
        "mk": [[key, label] for key, label in MARKERS],
        "sz": SZ,
        "set": {"uid": set_uid, "cols": ICONSET_COLS, "items": set_items},
    }
    html = HTML_SRC.read_text(encoding="utf-8")
    html = html.replace("__ICON_INDEX__", json.dumps(icon_index, separators=(",", ":")))
    html_gz = gzip.compress(html.encode("utf-8"), compresslevel=9, mtime=0)

    OUT_H.write_text(
        "// Generated by examples/tak_gateway/tools/build_web_assets.py - do not edit by hand.\n"
        "#pragma once\n#include <Arduino.h>\n\n"
        + c_array("TAK_INDEX_HTML_GZ", html_gz) + "\n\n"
        + c_array("TAK_ICONS_PNG", png) + "\n\n"
        + c_array("TAK_ICONSET_PNG", set_png) + "\n",
        encoding="utf-8",
    )
    print(f"index.html {len(html)} B -> {len(html_gz)} B gz; icons.png {len(png)} B; "
          f"iconset.png {len(set_png)} B ({len(set_items)} icons, uid {set_uid}) -> {OUT_H}")


if __name__ == "__main__":
    main()
