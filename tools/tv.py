#!/usr/bin/env python3
"""Minimal telnet runner: tv.py 'cmd' [timeout]. Prints command output.
The TV's address comes from $NETCAST_TV, or from the file tools/.tv-host (one line, not committed)."""
import os, socket, sys, time, re

def tv_host():
    h = os.environ.get("NETCAST_TV", "").strip()
    if not h:
        try:
            with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".tv-host")) as f:
                h = f.read().strip()
        except OSError:
            pass
    if not h:
        sys.exit("tv.py: set NETCAST_TV=<TV IP> or write the IP into tools/.tv-host")
    return h

PORT = 23
IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240

def strip_iac(sock, data):
    out = bytearray(); i = 0
    while i < len(data):
        b = data[i]
        if b == IAC and i + 1 < len(data):
            c = data[i+1]
            if c in (DO, DONT, WILL, WONT) and i + 2 < len(data):
                opt = data[i+2]
                if c == DO: sock.send(bytes([IAC, WONT, opt]))
                elif c == WILL: sock.send(bytes([IAC, DONT, opt]))
                i += 3; continue
            if c == SB:
                j = data.find(bytes([IAC, SE]), i)
                i = j + 2 if j >= 0 else len(data); continue
            if c == IAC: out.append(IAC); i += 2; continue
            i += 2; continue
        out.append(b); i += 1
    return bytes(out)

def read_until(sock, pat, timeout):
    buf = b""; end = time.time() + timeout
    while time.time() < end:
        sock.settimeout(max(0.05, end - time.time()))
        try:
            d = sock.recv(65536)
        except socket.timeout:
            break
        if not d: break
        buf += strip_iac(sock, d)
        if re.search(pat, buf): break
    return buf

def main():
    cmd = sys.argv[1]; timeout = float(sys.argv[2]) if len(sys.argv) > 2 else 20
    s = socket.create_connection((tv_host(), PORT), timeout=10)
    banner = read_until(s, rb"[#$>] ?$|login: ?$", 5)
    if b"login" in banner[-20:]:
        s.send(b"root\r\n"); read_until(s, rb"[#$] ?$", 5)
    mark = "__END_%d__" % int(time.time()*1000)
    a, b = mark[:5], mark[5:]
    s.send(f"echo {a}''S{b}; {cmd}; echo; echo {a}''E{b}\r\n".encode())
    out = read_until(s, (a+"E"+b).encode() + rb"\r?\n", timeout)
    s.close()
    txt = out.decode("utf-8", "replace").replace("\r", "")
    # drop echoed command line(s) & marker
    st, en = a+"S"+b, a+"E"+b
    if st in txt: txt = txt.split(st, 1)[1]
    if en in txt: txt = txt.split(en, 1)[0]
    print(txt.strip("\n"))

if __name__ == "__main__":
    main()
