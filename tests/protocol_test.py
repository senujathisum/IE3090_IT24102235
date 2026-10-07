#!/usr/bin/env python3
"""Black-box protocol tests for agent_235 (IT24102235).
Run from the directory where ./agent_235 is running:  python3 tests/protocol_test.py
Uses only the Python standard library."""
import socket, time, os, threading, hashlib, sys

HOST, PORT, SID = "127.0.0.1", 9410, " SID:5322"
results = []

def check(name, cond, detail=""):
    results.append((name, bool(cond)))
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else "  -> " + str(detail)))

class Conn:
    def __init__(self):
        self.s = socket.create_connection((HOST, PORT))
        self.buf = b""
    def send(self, data):
        self.s.sendall(data if isinstance(data, bytes) else data.encode())
    def line(self, timeout=5):
        self.s.settimeout(timeout)
        while b"\n" not in self.buf:
            d = self.s.recv(65536)
            if not d:
                raise ConnectionError("closed")
            self.buf += d
        l, self.buf = self.buf.split(b"\n", 1)
        return l.decode()
    def exact(self, n):
        while len(self.buf) < n:
            d = self.s.recv(65536)
            if not d:
                raise ConnectionError("closed")
            self.buf += d
        out, self.buf = self.buf[:n], self.buf[n:]
        return out
    def cmd(self, c):
        self.send(c + "\n")
        return self.line()
    def closed(self):
        try:
            self.s.settimeout(3)
            return self.s.recv(10) == b""
        except Exception:
            return True

all_responses = []
def C():
    c = Conn()
    orig = c.line
    def tracked(timeout=5):
        l = orig(timeout); all_responses.append(l); return l
    c.line = tracked
    return c

# 1. pre-auth rejection
c = C()
check("SYSINFO before AUTH -> ERR 003", c.cmd("SYSINFO") == "ERR 003 NOT_AUTHENTICATED" + SID)
check("QUIT before AUTH -> ERR 003", c.cmd("QUIT") == "ERR 003 NOT_AUTHENTICATED" + SID)
check("PUT before AUTH drains bytes -> ERR 003",
      (c.send("PUT x.txt 5\nhello"), c.line())[1] == "ERR 003 NOT_AUTHENTICATED" + SID)
check("bad token -> ERR 001", c.cmd("AUTH OPS-0000") == "ERR 001 AUTH_FAILED" + SID)
check("good token -> OK AUTHENTICATED", c.cmd("AUTH OPS-2235") == "OK AUTHENTICATED" + SID)
check("second AUTH -> ERR 013", c.cmd("AUTH OPS-2235").startswith("ERR 013"))

# 2. three failures closes
d = C()
for _ in range(3):
    r = d.cmd("AUTH wrong")
check("3rd failed AUTH -> ERR 001 then connection closed", r == "ERR 001 AUTH_FAILED" + SID and d.closed())

# 3. SYSINFO / LISTPROC / EXEC
r = c.cmd("SYSINFO"); p = r.split()
check("SYSINFO format", len(p) == 6 and p[0:2] == ["OK", "SYSINFO"] and float(p[2]) >= 0
      and int(p[3]) > 0 and int(p[4]) > 0 and r.endswith(SID), r)
r = c.cmd("LISTPROC")
check("LISTPROC format", r.startswith("OK PROCS ") and r.endswith(SID) and "," in r and ":" in r, r[:100])
for name in ["DATE", "UPTIME", "DISKFREE", "HOSTNAME", "WHOAMI"]:
    r = c.cmd("EXEC " + name)
    check("EXEC %s -> OK EXEC_RESULT" % name, r.startswith("OK EXEC_RESULT ") and r.endswith(SID), r)
for bad in ["EXEC ls", "EXEC rm", "EXEC date", "EXEC DATE;rm"]:
    check(bad + " -> ERR 002", c.cmd(bad) == "ERR 002 COMMAND_NOT_ALLOWED" + SID)
check("EXEC no arg -> ERR 007", c.cmd("EXEC").startswith("ERR 007"))
check("unknown command -> ERR 006", c.cmd("FOO") == "ERR 006 UNKNOWN_COMMAND" + SID)

# 4. framing: partial line and multiple lines per buffer
c.send("SYS"); time.sleep(0.3); c.send("IN"); time.sleep(0.3); c.send("FO\n")
check("partial line across 3 sends", c.line().startswith("OK SYSINFO"))
c.send("EXEC WHOAMI\nEXEC HOSTNAME\nSYSINFO\n")
a, b, e = c.line(), c.line(), c.line()
check("3 commands in one send -> 3 responses in order",
      a.startswith("OK EXEC_RESULT") and b.startswith("OK EXEC_RESULT") and e.startswith("OK SYSINFO"))
c.send("EXEC DATE\r\n")
check("CRLF line ending accepted", c.line().startswith("OK EXEC_RESULT"))

# 5. PUT / GET binary integrity
data = os.urandom(1_234_567) + b"\n\n\x00tail\n"
c.send("PUT big.bin %d\n" % len(data))
for i in range(0, len(data), 7777):          # many small sends
    c.send(data[i:i + 7777])
check("PUT 1.2MB binary -> OK FILE_RECEIVED", c.line() == "OK FILE_RECEIVED big.bin" + SID)
stored = open("agentfiles/IT24102235/big.bin", "rb").read()
check("stored file byte-identical", stored == data)
r = c.cmd("GET big.bin")
check("GET header", r == "OK FILE_SEND big.bin %d" % len(data) + SID, r)
got = c.exact(len(data))
check("GET bytes byte-identical (md5)", hashlib.md5(got).hexdigest() == hashlib.md5(data).hexdigest())
check("stream in sync after GET", c.cmd("EXEC WHOAMI").startswith("OK EXEC_RESULT"))

# header + data + next command in ONE send
c.send(b"PUT tiny.txt 5\nHELLOSYSINFO\n")
check("PUT+data+next cmd in one buffer (1)", c.line() == "OK FILE_RECEIVED tiny.txt" + SID)
check("PUT+data+next cmd in one buffer (2)", c.line().startswith("OK SYSINFO"))
c.send(b"PUT empty.txt 0\n")
check("PUT empty file", c.line() == "OK FILE_RECEIVED empty.txt" + SID)
r = c.cmd("GET empty.txt")
check("GET empty file", r == "OK FILE_SEND empty.txt 0" + SID)
check("GET missing -> ERR 005", c.cmd("GET nope.bin") == "ERR 005 FILE_NOT_FOUND" + SID)
check("GET ../ -> ERR 008", c.cmd("GET ../agent_235.c") == "ERR 008 INVALID_FILENAME" + SID)
c.send("PUT ../evil.txt 3\nabc")
check("PUT ../ -> ERR 008 (bytes drained)", c.line() == "ERR 008 INVALID_FILENAME" + SID)
big = 10 * 1024 * 1024 + 1
c.send("PUT huge.bin %d\n" % big)
c.send(b"\0" * big)
check("PUT >10MB -> ERR 004", c.line(timeout=30) == "ERR 004 FILE_TOO_LARGE" + SID)
check("stream in sync after rejected PUT", c.cmd("EXEC WHOAMI").startswith("OK EXEC_RESULT"))
check("huge.bin not stored", not os.path.exists("agentfiles/IT24102235/huge.bin"))
check("PUT bad size -> ERR 007", c.cmd("PUT a.txt abc").startswith("ERR 007"))

# 6. UDP monitoring
u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); u.bind(("127.0.0.1", 50235)); u.settimeout(5)
check("MONITOR START -> OK", c.cmd("MONITOR START 50235") == "OK MONITOR_STARTED" + SID)
check("MONITOR START again -> ERR 009", c.cmd("MONITOR START 50235").startswith("ERR 009"))
t0 = time.time(); dgs = []
for _ in range(3):
    dgs.append(u.recv(512).decode())
gap = (time.time() - t0)
check("3 UDP datagrams received", len(dgs) == 3, dgs)
check("datagram format SYSINFO ... SID:5322", all(x.startswith("SYSINFO ") and x.endswith(SID) and len(x.split()) == 5 for x in dgs), dgs)
check("interval ~2s", 3.0 < gap < 6.5, gap)
check("TCP still works while monitoring", c.cmd("SYSINFO").startswith("OK SYSINFO"))
check("MONITOR STOP -> OK", c.cmd("MONITOR STOP") == "OK MONITOR_STOPPED" + SID)
u.settimeout(4.5)
try:
    u.recv(512); stopped = False
except socket.timeout:
    stopped = True
check("no datagrams after STOP", stopped)
check("MONITOR STOP again -> ERR 010", c.cmd("MONITOR STOP").startswith("ERR 010"))
check("MONITOR START bad port -> ERR 007", c.cmd("MONITOR START 99999").startswith("ERR 007"))

# 7. ungraceful disconnect while monitoring
e2 = C(); e2.cmd("AUTH OPS-2235"); e2.cmd("MONITOR START 50236")
e2.s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x01\x00\x00\x00\x00\x00\x00\x00")  # RST on close
e2.s.close()
time.sleep(2.5)
check("agent alive after ungraceful (RST) disconnect", C().cmd("AUTH OPS-2235").startswith("OK"))
e3 = C(); e3.cmd("AUTH OPS-2235"); e3.send("PUT part.bin 1000\nabc"); e3.s.close()
time.sleep(1)
check("disconnect mid-PUT: no partial file left",
      not any(f.startswith("part.bin") for f in os.listdir("agentfiles/IT24102235")))

# 8. concurrency: 6 clients at once, each holding the connection
ok = []
def worker(i):
    try:
        k = C()
        k.cmd("AUTH OPS-2235")
        time.sleep(1.0)                      # all connected simultaneously
        payload = os.urandom(200_000)
        k.send("PUT c%d.bin %d\n" % (i, len(payload))); k.send(payload)
        r1 = k.line()
        r2 = k.cmd("GET c%d.bin" % i); back = k.exact(len(payload))
        r3 = k.cmd("QUIT")
        ok.append(r1.startswith("OK FILE_RECEIVED") and back == payload and r3 == "OK BYE" + SID)
    except Exception as ex:
        ok.append(False); print(ex)
ths = [threading.Thread(target=worker, args=(i,)) for i in range(6)]
[t.start() for t in ths]; [t.join() for t in ths]
check("6 simultaneous controllers all succeed", len(ok) == 6 and all(ok), ok)

# 9. line too long
L = C()
L.send("A" * 3000 + "\n")
check("line too long -> ERR 012 and close", L.line() == "ERR 012 LINE_TOO_LONG" + SID and L.closed())

# 10. QUIT
check("QUIT -> OK BYE", c.cmd("QUIT") == "OK BYE" + SID)
check("connection closed after BYE", c.closed())

check("EVERY TCP response ends with ' SID:5322'", all(r.endswith(SID) for r in all_responses),
      [r for r in all_responses if not r.endswith(SID)][:3])

n_pass = sum(1 for _, p in results if p)
print("\n%d/%d tests passed" % (n_pass, len(results)))
sys.exit(0 if n_pass == len(results) else 1)
