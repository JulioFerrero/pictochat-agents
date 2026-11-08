# usage: python3 sendcodes.py PORT -> every 16-byte unit u (as bitmap_to_image places it) carries a 32-bit code of u
import json, socket, sys
def code(u): return (u & 0x3FF) | ((~u & 0x3FF) << 10) | (0xA5B << 20)
px = [[0] * 256 for _ in range(80)]
for u in range(640):
    t, half = divmod(u, 2); tx, ty = t % 32, t // 32
    c = code(u)
    for i in range(32):
        if (c >> i) & 1: px[ty * 8 + half * 4 + i // 8][tx * 8 + i % 8] = 1
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
s.sendall((json.dumps({"cmd": "chat", "pixels": px}) + "\n").encode()); s.close(); print("sent")
