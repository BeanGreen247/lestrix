"""A small FTP server for tests (stdlib only). Prints its port, serves a temp dir, quits on SIGTERM."""
import os, socket, socketserver, sys, threading, time

ROOT = sys.argv[1]


def fs(path):
    return os.path.normpath(os.path.join(ROOT, path.lstrip("/")))


class Handler(socketserver.StreamRequestHandler):
    def send(self, line):
        self.wfile.write((line + "\r\n").encode()); self.wfile.flush()

    def handle(self):
        self.cwd = "/"; self.pasv = None; self.rnfr = None
        self.send("220 test ftp")
        for raw in self.rfile:
            line = raw.decode().rstrip("\r\n"); cmd, _, arg = line.partition(" "); cmd = cmd.upper()
            if cmd == "USER": self.send("331 password please")
            elif cmd == "PASS": self.send("230 logged in")
            elif cmd == "SYST": self.send("215 UNIX Type: L8")
            elif cmd == "PWD": self.send('257 "%s"' % self.cwd)
            elif cmd == "TYPE": self.send("200 ok")
            elif cmd in ("CWD", "CDUP"):
                self.cwd = "/" if cmd == "CDUP" else (arg if arg.startswith("/") else os.path.join(self.cwd, arg))
                self.send("250 ok")
            elif cmd in ("EPSV", "EPRT", "FEAT", "OPTS"): self.send("500 not supported")
            elif cmd == "PASV":
                self.pasv = socket.socket(); self.pasv.bind(("127.0.0.1", 0)); self.pasv.listen(1)
                port = self.pasv.getsockname()[1]
                self.send("227 Entering Passive Mode (127,0,0,1,%d,%d)" % (port >> 8, port & 255))
            elif cmd == "LIST":
                path = fs(arg if arg and not arg.startswith("-") else self.cwd)
                self.send("150 listing"); conn, _ = self.pasv.accept()
                out = []
                for n in sorted(os.listdir(path)):
                    st = os.lstat(os.path.join(path, n)); d = "d" if os.path.isdir(os.path.join(path, n)) else "-"
                    out.append("%srwxr-xr-x 1 owner group %d Oct  3 12:00 %s" % (d, st.st_size, n))
                conn.sendall(("\r\n".join(out) + "\r\n").encode()); conn.close(); self.send("226 done")
            elif cmd == "RETR":
                self.send("150 sending"); conn, _ = self.pasv.accept()
                with open(fs(arg), "rb") as f: conn.sendall(f.read())
                conn.close(); self.send("226 done")
            elif cmd == "STOR":
                self.send("150 receiving"); conn, _ = self.pasv.accept(); data = b""
                while True:
                    chunk = conn.recv(65536)
                    if not chunk: break
                    data += chunk
                conn.close()
                with open(fs(arg), "wb") as f: f.write(data)
                self.send("226 done")
            elif cmd == "SIZE":
                try: self.send("213 %d" % os.path.getsize(fs(arg)))
                except OSError: self.send("550 no such file")
            elif cmd == "MKD": os.makedirs(fs(arg), exist_ok=True); self.send('257 "%s" created' % arg)
            elif cmd == "RMD":
                try: os.rmdir(fs(arg)); self.send("250 removed")
                except OSError as e: self.send("550 %s" % e)
            elif cmd == "DELE":
                try: os.remove(fs(arg)); self.send("250 deleted")
                except OSError as e: self.send("550 %s" % e)
            elif cmd == "RNFR": self.rnfr = arg; self.send("350 ready")
            elif cmd == "RNTO": os.rename(fs(self.rnfr), fs(arg)); self.send("250 renamed")
            elif cmd == "NOOP": self.send("200 ok")
            elif cmd == "QUIT": self.send("221 bye"); return
            else: self.send("502 not implemented")


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


srv = Server(("127.0.0.1", 0), Handler)
print(srv.server_address[1], flush=True)
srv.serve_forever()
