#!/usr/bin/env python3
"""Sparkle/WinSparkle appcast for a release (B7). One item per release, one enclosure per installer, each signed with
the EdDSA (Ed25519) key both updaters check. The key is Sparkle's base64 export (seed, or seed+public).

  make_appcast.py keygen                                   # prints SPARKLE_ED_PRIVATE_KEY and the public key
  SPARKLE_ED_PRIVATE_KEY=... make_appcast.py build --version 0.3.0 --url-base https://.../download/v0.3.0 \
      --notes https://.../releases/tag/v0.3.0 SceneVisualiser.msi SceneVisualiser.dmg > appcast.xml
  make_appcast.py --self-test
"""
import argparse
import base64
import email.utils
import os
import sys
import tempfile
import xml.etree.ElementTree as ET

try:
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    from cryptography.hazmat.primitives.serialization import Encoding, NoEncryption, PrivateFormat, PublicFormat
except ImportError:
    print("make_appcast needs the 'cryptography' package (pip install cryptography)", file=sys.stderr)
    sys.exit(77)  # ctest skip

SPARKLE = "http://www.andymatuschak.org/xml-namespaces/sparkle"
OS_OF = {".msi": "windows", ".exe": "windows", ".dmg": "macos", ".zip": "macos"}


def private_key(b64):
    return Ed25519PrivateKey.from_private_bytes(base64.b64decode(b64)[:32])


def private_b64(key):
    return base64.b64encode(key.private_bytes(Encoding.Raw, PrivateFormat.Raw, NoEncryption())).decode()


def public_b64(key):
    return base64.b64encode(key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)).decode()


def appcast(version, url_base, notes, files, key):
    ET.register_namespace("sparkle", SPARKLE)
    rss = ET.Element("rss", {"version": "2.0"})
    channel = ET.SubElement(rss, "channel")
    ET.SubElement(channel, "title").text = "Scene Visualiser"
    item = ET.SubElement(channel, "item")
    ET.SubElement(item, "title").text = f"Version {version}"
    ET.SubElement(item, "pubDate").text = email.utils.formatdate(usegmt=True)
    ET.SubElement(item, f"{{{SPARKLE}}}version").text = version
    ET.SubElement(item, f"{{{SPARKLE}}}shortVersionString").text = version
    if notes:
        ET.SubElement(item, f"{{{SPARKLE}}}releaseNotesLink").text = notes
    for path in files:
        data = open(path, "rb").read()
        ET.SubElement(item, "enclosure", {
            "url": f"{url_base.rstrip('/')}/{os.path.basename(path)}",
            "length": str(len(data)),
            "type": "application/octet-stream",
            f"{{{SPARKLE}}}os": OS_OF[os.path.splitext(path)[1].lower()],
            f"{{{SPARKLE}}}edSignature": base64.b64encode(key.sign(data)).decode(),
        })
    return '<?xml version="1.0" encoding="utf-8"?>\n' + ET.tostring(rss, encoding="unicode") + "\n"


def self_test():
    key = Ed25519PrivateKey.generate()
    exported = private_b64(key)
    with tempfile.TemporaryDirectory() as tmp:
        files = [os.path.join(tmp, n) for n in ("SceneVisualiser.msi", "SceneVisualiser.dmg")]
        for i, f in enumerate(files):
            open(f, "wb").write(bytes(range(256)) * (i + 3))
        xml = appcast("9.9.9-test", "https://example.org/dl/", "", files, private_key(exported))
    root = ET.fromstring(xml)
    enclosures = root.findall("./channel/item/enclosure")
    assert [e.get(f"{{{SPARKLE}}}os") for e in enclosures] == ["windows", "macos"], xml
    assert enclosures[0].get("url") == "https://example.org/dl/SceneVisualiser.msi", xml
    assert enclosures[1].get("length") == str(256 * 4), xml
    pub = Ed25519PrivateKey.from_private_bytes(base64.b64decode(exported)).public_key()
    pub.verify(base64.b64decode(enclosures[1].get(f"{{{SPARKLE}}}edSignature")), bytes(range(256)) * 4)
    assert root.find(f"./channel/item/{{{SPARKLE}}}version").text == "9.9.9-test"
    print("make_appcast self-test passed")


def main():
    if sys.argv[1:] == ["--self-test"]:
        return self_test()
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("keygen")
    build = sub.add_parser("build")
    build.add_argument("--version", required=True)
    build.add_argument("--url-base", required=True)
    build.add_argument("--notes", default="")
    build.add_argument("files", nargs="+")
    args = parser.parse_args()
    if args.cmd == "keygen":
        key = Ed25519PrivateKey.generate()
        print("SPARKLE_ED_PRIVATE_KEY=" + private_b64(key))
        print("SV_UPDATE_PUBLIC_KEY=" + public_b64(key))
        return
    sys.stdout.write(appcast(args.version, args.url_base, args.notes, args.files,
                             private_key(os.environ["SPARKLE_ED_PRIVATE_KEY"])))


if __name__ == "__main__":
    main()
