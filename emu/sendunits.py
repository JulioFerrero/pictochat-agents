# usage: python3 sendunits.py PORT u1 u2 ...  -> solid 8x4 block for each 16-byte unit u (as placed by bitmap_to_image)
import json, socket, sys
px = [[0] * 256 for _ in range(80)]
for u in map(int, sys.argv[2:]):
    t, half = divmod(u, 2); tx, ty = t % 32, t // 32
    for y in range(ty * 8 + half * 4, ty * 8 + half * 4 + 4):
        for x in range(tx * 8, tx * 8 + 8): px[y][x] = 1
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
s.sendall((json.dumps({"cmd": "chat", "pixels": px}) + "\n").encode()); s.close(); print("sent")
