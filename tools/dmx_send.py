#!/usr/bin/env python3
"""Test DMX sender (stdlib only): Art-Net 4 or sACN E1.31 patterns at a fixed rate.

  python3 tools/dmx_send.py --proto artnet --universes 1-8 --fps 44 --pattern chase --seconds 30
  python3 tools/dmx_send.py --proto sacn --host 239.255.0.1 --universes 1 --pattern full
Universes are 1-based project universes (Art-Net Port-Address = universe - 1).
"""
import argparse, math, socket, struct, time, uuid


def artdmx(universe, seq, data):
    pa = universe - 1
    return b"Art-Net\0" + struct.pack("<H", 0x5000) + bytes([0, 14, seq, 0, pa & 0xFF, (pa >> 8) & 0x7F]) + \
        struct.pack(">H", len(data)) + data


def sacn(cid, universe, seq, data, priority, name=b"dmx_send"):
    n = len(data)
    root = struct.pack(">HH12sHI16s", 0x10, 0, b"ASC-E1.17\0\0\0", 0x7000 | (110 + n), 4, cid)
    frame = struct.pack(">HI64sBHBBH", 0x7000 | (88 + n), 2, name.ljust(64, b"\0"), priority, 0, seq, 0, universe)
    dmp = struct.pack(">HBBHHHB", 0x7000 | (11 + n), 2, 0xA1, 0, 1, n + 1, 0)
    return root + frame + dmp + data


def pattern(name, t, universe):
    if name == "full":
        return bytes([255] * 512)
    if name == "chase":
        step = int(t * 4) % 64
        return bytes(255 if (i // 8) == step else 0 for i in range(512))
    # ramp: every slot sweeps 0..255, phase-shifted per universe
    return bytes(int(127.5 + 127.5 * math.sin(t * 2 + i * 0.05 + universe)) for i in range(512))


def universes(spec):
    out = []
    for part in spec.split(","):
        a, _, b = part.partition("-")
        out += range(int(a), int(b or a) + 1)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--proto", choices=["artnet", "sacn"], default="artnet")
    ap.add_argument("--host", help="unicast/broadcast IP; default 127.0.0.1 for Art-Net, multicast for sACN")
    ap.add_argument("--port", type=int)
    ap.add_argument("--universes", default="1")
    ap.add_argument("--fps", type=float, default=44)
    ap.add_argument("--seconds", type=float, default=0, help="0 = forever")
    ap.add_argument("--pattern", choices=["full", "chase", "ramp"], default="ramp")
    ap.add_argument("--priority", type=int, default=100)
    args = ap.parse_args()

    port = args.port or (6454 if args.proto == "artnet" else 5568)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    cid, unis, seq, sent = uuid.uuid4().bytes, universes(args.universes), 0, 0
    start = time.monotonic()
    while not args.seconds or time.monotonic() - start < args.seconds:
        t = time.monotonic() - start
        seq = seq % 255 + 1  # Art-Net: 1..255, 0 would disable sequencing
        for u in unis:
            data = pattern(args.pattern, t, u)
            if args.proto == "artnet":
                sock.sendto(artdmx(u, seq, data), (args.host or "127.0.0.1", port))
            else:
                host = args.host or f"239.255.{u >> 8}.{u & 0xFF}"
                sock.sendto(sacn(cid, u, seq, data, args.priority), (host, port))
            sent += 1
        time.sleep(max(0.0, start + (sent / len(unis)) / args.fps - time.monotonic()))
    print(f"sent {sent} packets")


if __name__ == "__main__":
    main()
