# Design Diary – RemoteOps (IT24102235)

Key decisions and obstacles, in order.


### 2026-10-07 – Personalisation

- My registration number IT24102235 has the standard format, so the formulas from §2.4 apply directly:
  - port 7000 + 2410 = **9410**
  - last three digits 235 → `agent_235.c`, `controller_235.c`, `Makefile_235`
  - last four digits 2235, reversed → **SID:5322**
  - token **OPS-2235**
  - log `remoteops_IT24102235.log`
  - storage `./agentfiles/IT24102235/`
- Every value is a `#define` at the top of `agent_235.c`, so it is easy to check and change.

### 2026-10-07 – Starting point

- I used an AI-generated reference implementation (see `PROMPT_LOG.md`) as a guide.
- I built my submission from scratch in a new repository on my CentOS Stream 10 VM (VMware Workstation Pro), typing each file myself.

### 2026-10-07 – Concurrency model: thread per connection

- **Options considered:**
  - `fork()` per client (Labs 3, 5, 8)
  - `select()` (Lab 4)
  - `pthread` per client (Lab 9)
- **Chosen:** thread per client. `MONITOR` needs a second activity per session: sending UDP every 2 s while the session waits for TCP commands. With threads, that is just one more thread that shares the session data.
  - With `fork()`, stopping a child's monitor would need signals.
  - With `select()`, every file transfer would have to be non-blocking.
- **Cost:** shared data must be protected, so the log file uses a mutex.

### 2026-10-07 – Framing (TCP is a byte stream)

- Lab 7 showed that one `send()` can arrive as several `recv()` chunks, or several messages can arrive in one chunk.
- So each connection keeps a receive buffer. `read_line()` returns one full line and keeps any extra bytes for next time.
- PUT/GET use the leftover bytes first, then loop on `recv()` until exactly `<filesize>` bytes have arrived (like `receive_all()` in Lab 5).
- If the Agent rejects a PUT, it still reads the declared number of bytes, so the next command is not mixed up with file data.

### 2026-10-07 – Security decisions

- EXEC only accepts the five names. The real commands are hard-coded, so no user text reaches the shell.
- Filenames may only use `A-Z a-z 0-9 . _ -`, which blocks `../` path traversal.
- Three wrong AUTH attempts close the connection. The token is masked in the log.

### 2026-10-07 – Disconnects

- `recv()` returning 0 (normal close) or −1 (reset) ends the session thread. It stops monitoring, closes the socket and frees memory.
- `SIGPIPE` is ignored, so writing to a dead client cannot kill the whole Agent.

### 2026-10-07 – Environment set-up on CentOS

- Installed `gcc`, `make` and `git` with `dnf`.
- Connected the VM to GitHub with an SSH key (`ssh-keygen`, as in Lab 6) and added the public key to my GitHub account.
- **Obstacle:** after building, the binaries and runtime files (`agentfiles/`, the log) showed up in `git status`. Fixed by adding a `.gitignore` so only source and documents are committed.

### 2026-10-07 – Build and testing on CentOS Stream 10

- `make -f Makefile_235` built both programs with **no warnings** under `-Wall -Wextra`.
- `./agent_235` listened on port 9410, and the Controller connected from 127.0.0.1.
- Tested by hand:
  - AUTH errors, then success
  - SYSINFO and LISTPROC
  - all five EXEC names, plus a rejected one
  - PUT/GET: `md5sum` of the original, downloaded and stored files were identical
  - MONITOR START/STOP
  - five controllers at once
  - Ctrl+C on a controller: the Agent kept running
