#!/usr/bin/env python3
"""Live view of the emulated DSes. Shows both screens of each instance; click on a bottom screen to tap it.
Usage: python3 viewer.py [instances=2]   (the harness must be running with its command fifo cmd.fifo next to this file)"""
import os
import sys
import time
import tkinter as tk

HERE = os.path.dirname(os.path.abspath(__file__))
FIFO = os.path.join(HERE, "cmd.fifo")
N = int(sys.argv[1]) if len(sys.argv) > 1 else 2
SCALE = 2


def send(line):
    try:
        fd = os.open(FIFO, os.O_WRONLY | os.O_NONBLOCK)
        os.write(fd, (line + "\n").encode())
        os.close(fd)
    except OSError:
        pass


class View:
    def __init__(self, root):
        self.root = root
        root.title("Emulated DS (click the lower screen to tap)")
        self.imgs, self.labels = [], []
        for i in range(N):
            f = tk.Frame(root)
            f.pack(side="left", padx=6, pady=6)
            tk.Label(f, text="DS %d" % i).pack()
            lab = tk.Label(f, width=256 * SCALE, height=384 * SCALE, bd=0)
            lab.pack()
            lab.bind("<Button-1>", lambda e, i=i: self.tap(i, e))
            self.labels.append(lab)
            self.imgs.append(None)
        self.n = 0
        self.tick()

    def tap(self, i, e):
        x, y = e.x // SCALE, e.y // SCALE - 192
        if 0 <= x < 256 and 0 <= y < 192:
            send("touch %d %d %d 8" % (i, x, y))

    def tick(self):
        for i in range(N):
            path = os.path.join(HERE, "live%d.ppm" % i)
            try:
                img = tk.PhotoImage(file=path)
                self.imgs[i] = img.zoom(SCALE, SCALE)
                self.labels[i].configure(image=self.imgs[i], width=256 * SCALE, height=384 * SCALE)
            except tk.TclError:
                pass
            send("shot %d %s" % (i, path))
        self.root.after(300, self.tick)


root = tk.Tk()
View(root)
root.mainloop()
