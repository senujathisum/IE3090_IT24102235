# RemoteOps – Remote System Monitoring & Management Tool (IE3090)

**Module:** IE3090 – Network Programming (Year 3, Semester 1)
**Registration number:** **IT24102235**

RemoteOps has two C programs:

- **Agent** (`agent_235.c`): a TCP server that runs on the managed machine.
- **Controller** (`controller_235.c`): the administrator's TCP client.

They talk over TCP using the fixed line-based protocol from the brief (§2.3). The Agent also sends periodic system statistics to the Controller over UDP (`MONITOR START/STOP`).

---

## 1. Personalised values (from IT24102235)

| Item | Formula (brief §2.4) | Calculation | Value |
|---|---|---|---|
| Numeric part | – | IT**24102235** | `24102235` |
| Agent listening port | 7000 + first four digits | 7000 + **2410** | **9410** |
| Source file names | `<last 3 digits>` | last three = **235** | `agent_235.c`, `controller_235.c`, `Makefile_235` |
| Session ID (SID) tag | last four digits reversed | **2235** → reversed | **`SID:5322`** |
| Authentication token | `OPS-` + last four digits | `OPS-` + **2235** | **`OPS-2235`** |
| Log file name | `remoteops_<full reg no>.log` | – | `remoteops_IT24102235.log` |
| File storage path | `./agentfiles/<full reg no>/<filename>` | – | `./agentfiles/IT24102235/<filename>` |
| Submission archive | `IE3090_<full reg no>.zip` | – | `IE3090_IT24102235.zip` |

My registration number has the same format as the example in the brief (`IT` + 8 digits), so I applied the formulas directly.

---

## 2. Build

You need `gcc` and `make` on Linux (CentOS Stream 10 lab machines: `sudo dnf install gcc make -y`).

```bash
make -f Makefile_235          # builds ./agent_235 and ./controller_235
make -f Makefile_235 clean    # removes the binaries
```

Both programs compile with `-Wall -Wextra` and produce **no warnings** (built and tested on CentOS Stream 10 in VMware Workstation Pro). They use only the standard BSD sockets API (`sys/socket.h`, `netinet/in.h`, `arpa/inet.h`) and POSIX threads (`pthread.h`).

## 3. Run

**Terminal 1 – Agent** (creates `./agentfiles/IT24102235/` and `remoteops_IT24102235.log` in the current directory):

```bash
./agent_235
```

**Terminal 2..N – Controller(s):**

```bash
./controller_235                    # connects to 127.0.0.1:9410
./controller_235 192.168.1.20       # Agent on another machine (port 9410)
./controller_235 192.168.1.20 9410  # explicit IP and port
```

Check that the Agent is listening on the personalised port:

```bash
ss -tlnp | grep 9410
```

If the Agent and Controller are on **different machines** with `firewalld` running (CentOS default), open the ports first:

```bash
sudo firewall-cmd --add-port=9410/tcp      # on the Agent machine
sudo firewall-cmd --add-port=6000/udp      # on the Controller machine (the MONITOR port you choose)
```

### Example session

```
remoteops> AUTH OPS-2235
<< OK AUTHENTICATED SID:5322
remoteops> SYSINFO
<< OK SYSINFO 0.20 607 123 SID:5322
remoteops> EXEC DATE
<< OK EXEC_RESULT Wed Oct  7 17:46:05 +0530 2026 SID:5322
remoteops> EXEC rm
<< ERR 002 COMMAND_NOT_ALLOWED SID:5322
remoteops> PUT report.pdf
>> PUT report.pdf 300000   (+ 300000 raw bytes)
<< OK FILE_RECEIVED report.pdf SID:5322
remoteops> GET report.pdf
<< OK FILE_SEND report.pdf 300000 SID:5322
   Saved 300000 bytes to ./downloads/report.pdf ...
remoteops> MONITOR START 6000
<< OK MONITOR_STARTED SID:5322
[UDP MONITOR from 127.0.0.1:57525] SYSINFO 0.20 607 123 SID:5322
remoteops> MONITOR STOP
<< OK MONITOR_STOPPED SID:5322
remoteops> QUIT
<< OK BYE SID:5322
```

### Controller commands

The Controller sends every line exactly as you type it, with three exceptions:

| You type | What the Controller does |
|---|---|
| `PUT <local_file>` | Finds the file size and sends `PUT <name> <size>\n`, then exactly `<size>` raw bytes. |
| `GET <filename>` | Sends `GET <filename>`, reads `OK FILE_SEND <name> <size>`, then reads exactly `<size>` bytes into `./downloads/<name>`. |
| `MONITOR START <port>` | Opens a UDP socket on `<port>` and starts a receiver thread **before** sending the command, so the first datagram is not lost. |
| `HELP` | Shows help locally. Nothing is sent. |

---

## 4. Protocol (implemented exactly as given in §2.3)

Every command and response is one line ending in `\n`. The exception is PUT/GET, where the line is followed immediately by the raw file bytes. **Every TCP response ends with ` SID:5322`**, and so does every UDP datagram.

| Command | Success response | Error responses |
|---|---|---|
| `AUTH <token>` | `OK AUTHENTICATED SID:5322` | `ERR 001 AUTH_FAILED`, `ERR 013 ALREADY_AUTHENTICATED` |
| `SYSINFO` | `OK SYSINFO <cpu_load> <mem_used_mb> <uptime_sec> SID:5322` | |
| `LISTPROC` | `OK PROCS <pid:name,pid:name,...> SID:5322` | |
| `EXEC <name>` | `OK EXEC_RESULT <output> SID:5322` | `ERR 002 COMMAND_NOT_ALLOWED` |
| `PUT <filename> <filesize>` + bytes | `OK FILE_RECEIVED <filename> SID:5322` | `ERR 004 FILE_TOO_LARGE`, `ERR 008 INVALID_FILENAME` |
| `GET <filename>` | `OK FILE_SEND <filename> <filesize> SID:5322` + bytes | `ERR 005 FILE_NOT_FOUND`, `ERR 008 INVALID_FILENAME` |
| `MONITOR START <udp_port>` | `OK MONITOR_STARTED SID:5322` | `ERR 009 MONITOR_ALREADY_RUNNING` |
| `MONITOR STOP` | `OK MONITOR_STOPPED SID:5322` | `ERR 010 MONITOR_NOT_RUNNING` |
| `QUIT` | `OK BYE SID:5322` | |

UDP datagram (every **2 seconds** after `MONITOR START`): `SYSINFO <cpu_load> <mem_used_mb> <uptime_sec> SID:5322`

### Error codes (`ERR <code> <REASON_CODE> SID:5322`)

| Code | Reason | When |
|---|---|---|
| 001 | `AUTH_FAILED` | Wrong token. After 3 failures the Agent closes the connection. |
| 002 | `COMMAND_NOT_ALLOWED` | `EXEC` name not in the whitelist |
| 003 | `NOT_AUTHENTICATED` | Any command other than `AUTH` before a successful `AUTH` |
| 004 | `FILE_TOO_LARGE` | `PUT` larger than 10 MB (10 485 760 bytes) |
| 005 | `FILE_NOT_FOUND` | `GET` of a file that was not uploaded |
| 006 | `UNKNOWN_COMMAND` | Command word not in the protocol |
| 007 | `INVALID_ARGUMENTS` | Wrong number or format of arguments (e.g. bad size or port) |
| 008 | `INVALID_FILENAME` | Name contains `/`, starts with `.`, or uses characters other than `A-Z a-z 0-9 . _ -` |
| 009 | `MONITOR_ALREADY_RUNNING` | `MONITOR START` while already monitoring |
| 010 | `MONITOR_NOT_RUNNING` | `MONITOR STOP` while not monitoring |
| 011 | `INTERNAL_ERROR` | Agent-side failure (popen/fopen/socket) |
| 012 | `LINE_TOO_LONG` | Command line over 1024 bytes without `\n`. The Agent then closes the connection. |
| 013 | `ALREADY_AUTHENTICATED` | `AUTH` sent again after success |

### EXEC whitelist (fixed – cannot be extended from the Controller)

| Name | Command run on the Agent |
|---|---|
| `DATE` | `date` |
| `UPTIME` | `uptime` |
| `DISKFREE` | `df -h /` |
| `HOSTNAME` | `uname -n` |
| `WHOAMI` | `whoami` |

The Controller only ever sends a *name*. The command text is hard-coded in the Agent, so Controller input never reaches the shell.

---

## 5. Design summary

- **Concurrency:** one POSIX thread per Controller connection (`pthread_create` + `pthread_detach`), plus one monitor thread per session while `MONITOR` is active. See the Implementation Report for the justification.
- **Framing:** each connection has its own receive buffer (`conn_reader`). `read_line()` handles a partial line spread over several `recv()` calls and several lines arriving in one `recv()`. `receive_file_data()` uses leftover buffered bytes first, then calls `recv()` until exactly `<filesize>` bytes are read. `send_all()` loops until every byte is sent.
- **One response function:** every OK/ERR line goes through `send_response()`, which appends ` SID:5322\n`, so no response can miss the tag.
- **Disconnects:** `recv()` returning 0 (FIN) or −1 (e.g. RST) ends the session thread cleanly. The monitor thread is stopped, the socket closed and memory freed. `SIGPIPE` is ignored so writing to a dead socket cannot kill the Agent.
- **Logging:** every connection, command, response, file transfer and disconnect goes to `remoteops_IT24102235.log` with a `[YYYY-MM-DD HH:MM:SS]` timestamp. A mutex protects the log because many threads write to it. The auth token is masked as `AUTH ********`.
- **Optional extension implemented:** transfer throughput (bytes/second) for PUT and GET. The Controller prints it, and the Agent writes it to the log.

## 6. Testing

`tests/protocol_test.py` (generated with AI help, see `PROMPT_LOG.md`) is a black-box test script (Python 3, standard library only) with 56 checks of the Agent's protocol behaviour. It covers partial lines, several commands in one buffer, binary file integrity, rejected uploads, UDP timing, ungraceful disconnects and 6 simultaneous clients. Run it from the directory where the Agent is running:

```bash
./agent_235 &
python3 tests/protocol_test.py
```

## 7. Repository contents

| File | Purpose |
|---|---|
| `agent_235.c` | Agent (server) source |
| `controller_235.c` | Controller (client) source |
| `Makefile_235` | Personalised Makefile |
| `README.md` | This file |
| `DESIGN_DIARY.md` | Design diary (decisions and obstacles) |
| `PROMPT_LOG.md` | Record of AI interactions |
| `REFLECTION.md` | Structured reflection |
| `tests/protocol_test.py` | Automated protocol tests (AI-generated, see PROMPT_LOG.md) |
| `sample_log/remoteops_IT24102235.log` | Sample excerpt of the Agent log |
