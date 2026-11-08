# usage: early.py PCAP N -> the first N frames after the first assoc-resp, all types, with key fields
import importlib.util, struct, sys
spec = importlib.util.spec_from_file_location("pc", __import__("os").path.join(__import__("os").path.dirname(__import__("os").path.abspath(__file__)), "..", "pictochat.py"))
pc = importlib.util.module_from_spec(spec); spec.loader.exec_module(pc)
fr = [f for f in (pc.parse_frame(ts, pkt, lt) for ts, pkt, lt in pc.read_pcap(sys.argv[1])) if f]
i = [k for k, f in enumerate(fr) if f.cls == "assoc-resp"][0]
t0 = fr[i].ts
for f in fr[i - 3:i + int(sys.argv[2])]:
    b = f.body[:-4] if f.cls.startswith("MP-") else f.body
    if f.cls == "beacon":
        g = [d for tg, d, _ in pc.parse_ies(f.body[12:]) if tg == 0xdd][0][8:]
        x = "tsf=%d int=%d gi=%s" % (struct.unpack_from("<Q", f.body, 0)[0], struct.unpack_from("<H", f.body, 8)[0], g[8:].hex())
    elif f.cls == "MP-CMD":
        x = "ct=%04x mask=%d rest=%s" % (struct.unpack_from("<H", b, 0)[0], struct.unpack_from("<H", b, 2)[0], b[4:16].hex())
    else:
        x = b[:16].hex()
    print("%+8.4f %-10s %s %s" % (f.ts - t0, f.cls, pc.mac(f.a2)[-5:] if f.a2 else "", x))
