# usage: python3 sendchat.py PORT "text"  -> sends a text chat message through the host's GUI port
import importlib.util, json, socket, sys
spec = importlib.util.spec_from_file_location("g", __import__("os").path.join(__import__("os").path.dirname(__import__("os").path.abspath(__file__)), "..", "pictochat_gui.py"))
g = importlib.util.module_from_spec(spec); spec.loader.exec_module(g)
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
s.sendall((json.dumps({"cmd": "chat", "pixels": g.text_to_pixels(sys.argv[2])}) + "\n").encode()); s.close()
print("sent")
