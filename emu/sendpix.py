# usage: python3 sendpix.py PORT pattern  -> sends a test picture through the host's GUI port
import json, socket, sys
W, H = 256, 80
px = [[0] * W for _ in range(H)]
for y in (20, 40, 60):                      # horizontal lines
    for x in range(W): px[y][x] = 1
for x in (8, 64, 128, 200):                 # vertical lines, x=8 only in the top half
    for y in range(H if x != 8 else 40): px[y][x] = 1
for i in range(80): px[i][i] = 1            # diagonal from top-left
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
s.sendall((json.dumps({"cmd": "chat", "pixels": px}) + "\n").encode()); s.close(); print("sent")
