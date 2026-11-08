# usage: flow.py PCAP T_START T_END [--no-fcs] -> compressed host/client packet flow
# emulator captures carry 4 FCS bytes per body; pass --no-fcs for captures from the real radio (host_log.pcap)
import importlib.util, struct, sys
spec = importlib.util.spec_from_file_location("pc", __import__("os").path.join(__import__("os").path.dirname(__import__("os").path.abspath(__file__)), "..", "pictochat.py"))
pc = importlib.util.module_from_spec(spec); spec.loader.exec_module(pc)
P, a, b = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
FCS = 0 if "--no-fcs" in sys.argv else 4
fr = [f for f in (pc.parse_frame(ts, pkt, lt) for ts, pkt, lt in pc.read_pcap(P)) if f]
t0 = fr[0].ts
ev = []
for f in fr:
    t = f.ts - t0
    if not (a <= t <= b):
        continue
    if f.cls == "MP-CMD":
        body = f.body[:len(f.body) - FCS]
        if len(body) < 4:
            continue
        rest = body[4:]; src = "H"; mask = struct.unpack_from("<H", body, 2)[0]
    elif f.cls == "MP-REPLY":
        rest = f.body[:len(f.body) - FCS]; src = "C"; mask = None
    elif f.cls in ("assoc-resp", "deauth", "auth", "assoc-req"):
        ev.append([t, (src := "*", f.cls), 1, t]); continue
    else:
        continue
    if len(rest) < 2:
        key = (src, "blank")
    else:
        w = struct.unpack_from("<H", rest, 0)[0]; port = (w >> 8) & 0xF; ln = (w & 0xFF) * 2
        if ln == 0:
            key = (src, "idle")
        else:
            pl = rest[2:2 + ln]; ty = struct.unpack_from("<H", pl, 0)[0]
            if port == 0xE:
                _t, _l, mid, x, flen, last, off = struct.unpack_from("<HHBBBBI", pl, 0)
                key = (src, "E id=%d off=%d len=%d last=%d data=%s" % (mid, off, flen, last, pl[12:14].hex()))
            elif port == 0xD:
                key = (src, "D%d id=%d +8=%d +12=%d" % (ty, pl[4], struct.unpack_from("<I", pl, 8)[0], pl[12]))
            else:
                key = (src, "C%d" % ty)
    if src == "C" and key[1] in ("blank", "idle"):
        continue
    if ev and ev[-1][1] == key:
        ev[-1][2] += 1; ev[-1][3] = t
    else:
        ev.append([t, key, 1, t])
for e in ev:
    print("%8.3f-%8.3f %s %s x%d" % (e[0], e[3], e[1][0], e[1][1], e[2]))
