/*
 * agent_235.c - RemoteOps Agent (TCP server + UDP monitoring sender)
 *
 * IE3090 Network Programming - Assignment (Part 1)
 * Registration number : IT24102235
 *
 * Personalised values (derived from IT24102235, see assignment section 2.4):
 *   Listening port   : 7000 + 2410          = 9410
 *   Source files     : last three digits 235 -> agent_235.c, controller_235.c, Makefile_235
 *   SID tag          : last four 2235 reversed -> SID:5322
 *   Auth token       : "OPS-" + last four    = OPS-2235
 *   Log file         : remoteops_IT24102235.log
 *   Storage path     : ./agentfiles/IT24102235/<filename>
 *
 * Concurrency model : one POSIX thread per Controller connection
 *                     (+ one extra thread per session while MONITOR is active).
 *
 * Build : make -f Makefile_235
 * Run   : ./agent_235
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ---------------- Personalised values (IT24102235) ---------------- */
#define REG_NUMBER      "IT24102235"
#define AGENT_PORT      9410                    /* 7000 + 2410            */
#define SID_TAG         "SID:5322"              /* "2235" reversed        */
#define AUTH_TOKEN      "OPS-2235"              /* "OPS-" + last four     */
#define LOG_FILE        "remoteops_IT24102235.log"
#define STORAGE_ROOT    "./agentfiles"
#define STORAGE_DIR     "./agentfiles/IT24102235"

/* ---------------- Other settings ---------------- */
#define BACKLOG             10
#define MAX_LINE            1024        /* longest command line accepted      */
#define READ_BUF_SIZE       4096        /* per-connection receive buffer      */
#define FILE_CHUNK          4096        /* bytes per send()/recv() in files   */
#define MAX_FILE_SIZE       (10L * 1024 * 1024)   /* 10 MB upload limit      */
#define MAX_FILENAME        100
#define MAX_AUTH_ATTEMPTS   3
#define MONITOR_INTERVAL    2           /* seconds between UDP datagrams      */
#define MAX_RESPONSE        65536       /* longest response line we build     */
#define MAX_TOKENS          4

/* ---------------- Error codes (ERR <code> <REASON>) ---------------- */
#define E_AUTH_FAILED       "001 AUTH_FAILED"
#define E_NOT_ALLOWED       "002 COMMAND_NOT_ALLOWED"
#define E_NOT_AUTH          "003 NOT_AUTHENTICATED"
#define E_TOO_LARGE         "004 FILE_TOO_LARGE"
#define E_NOT_FOUND         "005 FILE_NOT_FOUND"
#define E_UNKNOWN           "006 UNKNOWN_COMMAND"
#define E_BAD_ARGS          "007 INVALID_ARGUMENTS"
#define E_BAD_FILENAME      "008 INVALID_FILENAME"
#define E_MON_RUNNING       "009 MONITOR_ALREADY_RUNNING"
#define E_MON_NOT_RUNNING   "010 MONITOR_NOT_RUNNING"
#define E_INTERNAL          "011 INTERNAL_ERROR"
#define E_LINE_TOO_LONG     "012 LINE_TOO_LONG"
#define E_ALREADY_AUTH      "013 ALREADY_AUTHENTICATED"

/*
 * Receive buffer for one TCP connection.
 * TCP is a byte stream, so one recv() may return half a line, or several
 * lines, or a line plus the start of file data. Bytes that have been
 * received but not used yet stay in buf[0..len-1].
 */
typedef struct {
    int  fd;
    char buf[READ_BUF_SIZE];
    int  len;
} conn_reader;

/* Everything the Agent knows about one Controller connection (session). */
typedef struct {
    int                 client_id;        /* 1, 2, 3 ... (for the log)       */
    int                 sock;             /* TCP socket to the Controller    */
    struct sockaddr_in  addr;             /* Controller IP/port from accept  */
    char                peer[64];         /* "ip:port" text for logging      */
    conn_reader         reader;
    int                 authenticated;
    int                 auth_attempts;
    int                 quit_received;    /* set when QUIT is processed      */

    /* UDP monitoring stream */
    int                 monitor_running;  /* protected by monitor_lock       */
    pthread_mutex_t     monitor_lock;
    pthread_t           monitor_tid;
    int                 udp_sock;
    struct sockaddr_in  udp_dest;         /* Controller IP + <udp_port>      */
} session_t;

/* Shared by all threads -> protected by a mutex */
static FILE           *log_fp = NULL;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

/* ================================================================== */
/*                              Logging                               */
/* ================================================================== */

/* Write "[YYYY-MM-DD HH:MM:SS] message" to the log file and the screen. */
static void log_event(const char *fmt, ...)
{
    char       msg[1024];
    char       stamp[32];
    time_t     now = time(NULL);
    struct tm  tm_now;
    va_list    ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    localtime_r(&now, &tm_now);               /* thread-safe localtime */
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_now);

    pthread_mutex_lock(&log_lock);            /* one writer at a time */
    if (log_fp != NULL) {
        fprintf(log_fp, "[%s] %s\n", stamp, msg);
        fflush(log_fp);
    }
    printf("[%s] %s\n", stamp, msg);
    fflush(stdout);
    pthread_mutex_unlock(&log_lock);
}

/* ================================================================== */
/*                  Sending / receiving helpers (framing)             */
/* ================================================================== */

/* send() may send fewer bytes than asked, so loop until all are sent. */
static int send_all(int sock, const char *data, long len)
{
    long total = 0;

    while (total < len) {
        ssize_t n = send(sock, data + total, len - total, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        total += n;
    }
    return 0;
}

/*
 * Read one '\n'-terminated line into 'line' (without the '\n' / '\r').
 * Returns  length of the line (>= 0)
 *          -1 if the peer disconnected or recv() failed
 *          -2 if no '\n' was found within MAX_LINE bytes (line too long)
 */
static int read_line(conn_reader *r, char *line, int max)
{
    while (1) {
        /* 1. Is there already a complete line in the buffer? */
        char *nl = memchr(r->buf, '\n', r->len);
        if (nl != NULL) {
            int line_len = (int)(nl - r->buf);
            int used     = line_len + 1;          /* include the '\n' */

            if (line_len >= max)
                return -2;
            memcpy(line, r->buf, line_len);
            if (line_len > 0 && line[line_len - 1] == '\r')
                line_len--;                       /* accept "\r\n" too */
            line[line_len] = '\0';

            /* keep any extra bytes (next line / file data) for later */
            memmove(r->buf, r->buf + used, r->len - used);
            r->len -= used;
            return line_len;
        }

        /* 2. No full line yet - the line must not be longer than max */
        if (r->len >= max || r->len == (int)sizeof(r->buf))
            return -2;

        /* 3. Receive more bytes (may be a partial line) */
        ssize_t n = recv(r->fd, r->buf + r->len, sizeof(r->buf) - r->len, 0);
        if (n == 0)
            return -1;                           /* orderly disconnect */
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;                           /* e.g. ECONNRESET    */
        }
        r->len += (int)n;
    }
}

/*
 * Receive exactly 'size' bytes of file data that follow a PUT line.
 * Bytes already sitting in the reader buffer are used first, then recv()
 * is called as many times as needed. If fp is NULL the bytes are read and
 * thrown away (used to stay in sync when a PUT is rejected).
 * Returns 0 on success, -1 if the connection broke, -2 if fwrite failed.
 */
static int receive_file_data(conn_reader *r, FILE *fp, long size)
{
    char chunk[FILE_CHUNK];
    long remaining = size;
    int  write_error = 0;

    while (remaining > 0) {
        long want = remaining < FILE_CHUNK ? remaining : FILE_CHUNK;
        long got;

        if (r->len > 0) {                         /* leftover bytes first */
            got = r->len < want ? r->len : want;
            memcpy(chunk, r->buf, got);
            memmove(r->buf, r->buf + got, r->len - got);
            r->len -= (int)got;
        } else {
            ssize_t n = recv(r->fd, chunk, want, 0);
            if (n == 0)
                return -1;
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            got = n;
        }

        if (fp != NULL && !write_error && fwrite(chunk, 1, got, fp) != (size_t)got)
            write_error = 1;      /* keep reading so the stream stays in sync */
        remaining -= got;
    }
    return write_error ? -2 : 0;
}

/*
 * Send one response line. EVERY TCP response goes through this function,
 * so every OK/ERR line is guaranteed to end with " SID:5322\n".
 */
static int send_response(session_t *s, const char *fmt, ...)
{
    char    line[MAX_RESPONSE + 32];
    va_list ap;
    int     len;

    va_start(ap, fmt);
    len = vsnprintf(line, MAX_RESPONSE, fmt, ap);
    va_end(ap);
    if (len >= MAX_RESPONSE)
        len = MAX_RESPONSE - 1;

    len += snprintf(line + len, sizeof(line) - len, " %s\n", SID_TAG);

    /* log the response (long PROCS lines are shortened in the log) */
    if (len > 200)
        log_event("Client %d <- %.150s... (%d bytes)", s->client_id, line, len);
    else
        log_event("Client %d <- %.*s", s->client_id, len - 1, line);

    return send_all(s->sock, line, len);
}

static int send_error(session_t *s, const char *code_and_reason)
{
    return send_response(s, "ERR %s", code_and_reason);
}

/* ================================================================== */
/*                       System information                           */
/* ================================================================== */

/* cpu_load = 1-minute load average, mem_used_mb = MemTotal - MemAvailable,
 * uptime_sec = seconds since boot. All read from Linux /proc files. */
static void get_sysinfo(double *cpu_load, long *mem_used_mb, long *uptime_sec)
{
    FILE  *fp;
    char   line[256];
    long   mem_total_kb = 0, mem_avail_kb = 0;
    double up = 0.0;

    *cpu_load = 0.0;
    *mem_used_mb = 0;
    *uptime_sec = 0;

    fp = fopen("/proc/loadavg", "r");
    if (fp != NULL) {
        if (fscanf(fp, "%lf", cpu_load) != 1)
            *cpu_load = 0.0;
        fclose(fp);
    }

    fp = fopen("/proc/meminfo", "r");
    if (fp != NULL) {
        while (fgets(line, sizeof(line), fp) != NULL) {
            sscanf(line, "MemTotal: %ld kB", &mem_total_kb);
            sscanf(line, "MemAvailable: %ld kB", &mem_avail_kb);
        }
        fclose(fp);
        *mem_used_mb = (mem_total_kb - mem_avail_kb) / 1024;
    }

    fp = fopen("/proc/uptime", "r");
    if (fp != NULL) {
        if (fscanf(fp, "%lf", &up) == 1)
            *uptime_sec = (long)up;
        fclose(fp);
    }
}

/* ================================================================== */
/*                         Helper functions                           */
/* ================================================================== */

/* Split a command line into space-separated tokens (thread-safe). */
static int split_tokens(char *line, char *tokens[], int max_tokens)
{
    char *save = NULL;
    int   count = 0;
    char *tok = strtok_r(line, " \t", &save);

    while (tok != NULL) {
        if (count == max_tokens)
            return max_tokens + 1;                /* too many tokens */
        tokens[count++] = tok;
        tok = strtok_r(NULL, " \t", &save);
    }
    return count;
}

/* Only allow plain names like report.pdf - no '/', no "..", no hidden
 * files - so a Controller can never read/write outside STORAGE_DIR. */
static int valid_filename(const char *name)
{
    size_t len = strlen(name);

    if (len == 0 || len > MAX_FILENAME || name[0] == '.')
        return 0;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

/* Parse a non-negative whole number such as a file size or port. */
static int parse_number(const char *text, long *value)
{
    char *end = NULL;

    if (text[0] < '0' || text[0] > '9')
        return 0;
    errno = 0;
    *value = strtol(text, &end, 10);
    return errno == 0 && *end == '\0';
}

static double seconds_since(struct timeval *start)
{
    struct timeval now;

    gettimeofday(&now, NULL);
    return (now.tv_sec - start->tv_sec) + (now.tv_usec - start->tv_usec) / 1e6;
}

/* ================================================================== */
/*                     UDP monitoring (MONITOR)                       */
/* ================================================================== */

static int monitor_is_running(session_t *s)
{
    int running;

    pthread_mutex_lock(&s->monitor_lock);
    running = s->monitor_running;
    pthread_mutex_unlock(&s->monitor_lock);
    return running;
}

/* Thread body: every MONITOR_INTERVAL seconds send one SYSINFO datagram
 * to the Controller's IP on the UDP port it asked for. */
static void *monitor_thread(void *arg)
{
    session_t *s = (session_t *)arg;
    char       datagram[256];
    double     cpu;
    long       mem, up;
    int        seq = 0;

    while (monitor_is_running(s)) {
        get_sysinfo(&cpu, &mem, &up);
        int len = snprintf(datagram, sizeof(datagram), "SYSINFO %.2f %ld %ld %s",
                           cpu, mem, up, SID_TAG);

        if (sendto(s->udp_sock, datagram, len, 0,
                   (struct sockaddr *)&s->udp_dest, sizeof(s->udp_dest)) < 0)
            log_event("Client %d UDP sendto failed: %s", s->client_id, strerror(errno));
        seq++;

        /* sleep in 1-second steps so MONITOR STOP is noticed quickly */
        for (int i = 0; i < MONITOR_INTERVAL && monitor_is_running(s); i++)
            sleep(1);
    }

    log_event("Client %d monitoring thread finished (%d datagrams sent)", s->client_id, seq);
    return NULL;
}

/* Stop the monitor thread (if any) and wait until it has really ended,
 * so no datagram can be sent after MONITOR_STOPPED / BYE. */
static void stop_monitor(session_t *s)
{
    if (!monitor_is_running(s))
        return;

    pthread_mutex_lock(&s->monitor_lock);
    s->monitor_running = 0;
    pthread_mutex_unlock(&s->monitor_lock);

    pthread_join(s->monitor_tid, NULL);
    close(s->udp_sock);
    s->udp_sock = -1;
}

static void cmd_monitor(session_t *s, char *tokens[], int ntok)
{
    long port;

    if (ntok == 3 && strcmp(tokens[1], "START") == 0) {
        if (!parse_number(tokens[2], &port) || port < 1 || port > 65535) {
            send_error(s, E_BAD_ARGS);
            return;
        }
        if (monitor_is_running(s)) {
            send_error(s, E_MON_RUNNING);
            return;
        }

        s->udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (s->udp_sock < 0) {
            send_error(s, E_INTERNAL);
            return;
        }
        /* Datagrams go to the same IP the TCP connection came from */
        memset(&s->udp_dest, 0, sizeof(s->udp_dest));
        s->udp_dest.sin_family = AF_INET;
        s->udp_dest.sin_addr   = s->addr.sin_addr;
        s->udp_dest.sin_port   = htons((unsigned short)port);

        pthread_mutex_lock(&s->monitor_lock);
        s->monitor_running = 1;
        pthread_mutex_unlock(&s->monitor_lock);
        if (pthread_create(&s->monitor_tid, NULL, monitor_thread, s) != 0) {
            s->monitor_running = 0;
            close(s->udp_sock);
            s->udp_sock = -1;
            send_error(s, E_INTERNAL);
            return;
        }
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &s->udp_dest.sin_addr, ip, sizeof(ip));
        log_event("Client %d MONITOR started -> %s:%ld/udp every %d s",
                  s->client_id, ip, port, MONITOR_INTERVAL);
        send_response(s, "OK MONITOR_STARTED");
    }
    else if (ntok == 2 && strcmp(tokens[1], "STOP") == 0) {
        if (!monitor_is_running(s)) {
            send_error(s, E_MON_NOT_RUNNING);
            return;
        }
        stop_monitor(s);
        log_event("Client %d MONITOR stopped", s->client_id);
        send_response(s, "OK MONITOR_STOPPED");
    }
    else {
        send_error(s, E_BAD_ARGS);
    }
}

/* ================================================================== */
/*                        Command handlers                            */
/* ================================================================== */

/* Returns 1 to keep the connection, 0 to close it. */
static int cmd_auth(session_t *s, char *tokens[], int ntok)
{
    if (s->authenticated) {
        send_error(s, E_ALREADY_AUTH);
        return 1;
    }
    if (ntok == 2 && strcmp(tokens[1], AUTH_TOKEN) == 0) {
        s->authenticated = 1;
        log_event("Client %d AUTH success", s->client_id);
        send_response(s, "OK AUTHENTICATED");
        return 1;
    }

    s->auth_attempts++;
    log_event("Client %d AUTH failed (attempt %d of %d)",
              s->client_id, s->auth_attempts, MAX_AUTH_ATTEMPTS);
    send_error(s, E_AUTH_FAILED);
    if (s->auth_attempts >= MAX_AUTH_ATTEMPTS) {
        log_event("Client %d too many failed AUTH attempts - closing connection",
                  s->client_id);
        return 0;
    }
    return 1;
}

static void cmd_sysinfo(session_t *s)
{
    double cpu;
    long   mem, up;

    get_sysinfo(&cpu, &mem, &up);
    send_response(s, "OK SYSINFO %.2f %ld %ld", cpu, mem, up);
}

/* Snapshot of running processes via popen("ps ..."), sent as
 * "pid:name,pid:name,..." on one line. */
static void cmd_listproc(session_t *s)
{
    char  list[MAX_RESPONSE - 256];
    char  line[512];
    int   used = 0, count = 0;
    FILE *fp = popen("ps -e -o pid=,comm=", "r");

    if (fp == NULL) {
        send_error(s, E_INTERNAL);
        return;
    }

    list[0] = '\0';
    while (fgets(line, sizeof(line), fp) != NULL) {
        int  pid;
        char name[256];

        line[strcspn(line, "\n")] = '\0';
        if (sscanf(line, "%d %255[^\n]", &pid, name) != 2)
            continue;
        for (char *p = name; *p; p++)              /* ',' is our separator */
            if (*p == ',')
                *p = '_';

        int need = snprintf(NULL, 0, "%s%d:%s", count ? "," : "", pid, name);
        if (used + need >= (int)sizeof(list) - 4) {
            strcat(list, ",...");                  /* list too long - cut */
            break;
        }
        used += sprintf(list + used, "%s%d:%s", count ? "," : "", pid, name);
        count++;
    }
    pclose(fp);

    log_event("Client %d LISTPROC returned %d processes", s->client_id, count);
    send_response(s, "OK PROCS %s", list);
}

/* EXEC whitelist: the Controller only sends a NAME; the actual command
 * text is fixed here, so no Controller input ever reaches the shell. */
static const char *whitelist_lookup(const char *name)
{
    if (strcmp(name, "DATE") == 0)     return "date";
    if (strcmp(name, "UPTIME") == 0)   return "uptime";
    if (strcmp(name, "DISKFREE") == 0) return "df -h /";
    if (strcmp(name, "HOSTNAME") == 0) return "uname -n";
    if (strcmp(name, "WHOAMI") == 0)   return "whoami";
    return NULL;
}

static void cmd_exec(session_t *s, char *tokens[], int ntok)
{
    char        output[4096];
    char        line[512];
    int         used = 0;
    const char *command;
    FILE       *fp;

    if (ntok != 2) {
        send_error(s, E_BAD_ARGS);
        return;
    }
    command = whitelist_lookup(tokens[1]);
    if (command == NULL) {
        log_event("Client %d EXEC '%s' rejected (not in whitelist)", s->client_id, tokens[1]);
        send_error(s, E_NOT_ALLOWED);
        return;
    }

    fp = popen(command, "r");
    if (fp == NULL) {
        send_error(s, E_INTERNAL);
        return;
    }
    /* The protocol is one line per response, so multi-line output
     * (e.g. df) is joined with " | " */
    output[0] = '\0';
    while (fgets(line, sizeof(line), fp) != NULL) {
        line[strcspn(line, "\n")] = '\0';
        if (line[0] == '\0')
            continue;
        used += snprintf(output + used, sizeof(output) - used, "%s%s",
                         used ? " | " : "", line);
        if (used >= (int)sizeof(output) - 1)
            break;
    }
    pclose(fp);

    if (output[0] == '\0')
        strcpy(output, "(no output)");
    log_event("Client %d EXEC %s -> '%s'", s->client_id, tokens[1], command);
    send_response(s, "OK EXEC_RESULT %s", output);
}

/*
 * PUT <filename> <filesize>, followed by exactly <filesize> raw bytes.
 * Rule: once a valid size is read, the Agent ALWAYS consumes exactly that
 * many bytes - even when it rejects the upload - so the next command line
 * is read from the correct place in the stream.
 * Returns 0 if the connection broke during the transfer, else 1.
 */
static int cmd_put(session_t *s, char *tokens[], int ntok)
{
    char           path[256], temp_path[300];
    long           size;
    FILE          *fp;
    struct timeval start;
    int            rc;

    if (ntok != 3 || !parse_number(tokens[2], &size)) {
        /* no valid size -> we cannot know how many bytes follow */
        send_error(s, s->authenticated ? E_BAD_ARGS : E_NOT_AUTH);
        return 1;
    }

    if (!s->authenticated || !valid_filename(tokens[1]) || size > MAX_FILE_SIZE) {
        /* reject, but first read and discard the file bytes */
        if (receive_file_data(&s->reader, NULL, size) < 0)
            return 0;
        if (!s->authenticated)
            send_error(s, E_NOT_AUTH);
        else if (!valid_filename(tokens[1]))
            send_error(s, E_BAD_FILENAME);
        else {
            log_event("Client %d PUT %s rejected: %ld bytes > limit %ld",
                      s->client_id, tokens[1], size, MAX_FILE_SIZE);
            send_error(s, E_TOO_LARGE);
        }
        return 1;
    }

    /* write to a temporary name first, then rename() when complete, so a
     * half-received file never appears under the real name */
    snprintf(path, sizeof(path), "%s/%s", STORAGE_DIR, tokens[1]);
    snprintf(temp_path, sizeof(temp_path), "%s.part%d", path, s->client_id);

    fp = fopen(temp_path, "wb");
    if (fp == NULL) {
        if (receive_file_data(&s->reader, NULL, size) < 0)
            return 0;
        send_error(s, E_INTERNAL);
        return 1;
    }

    gettimeofday(&start, NULL);
    rc = receive_file_data(&s->reader, fp, size);
    double secs = seconds_since(&start);
    fclose(fp);

    if (rc == -1) {                               /* client vanished */
        remove(temp_path);
        log_event("Client %d PUT %s aborted - connection lost", s->client_id, tokens[1]);
        return 0;
    }
    if (rc == -2 || rename(temp_path, path) != 0) {
        remove(temp_path);
        send_error(s, E_INTERNAL);
        return 1;
    }

    log_event("Client %d PUT %s stored at %s (%ld bytes in %.3f s, %.0f bytes/s)",
              s->client_id, tokens[1], path, size, secs,
              secs > 0 ? size / secs : (double)size);
    send_response(s, "OK FILE_RECEIVED %s", tokens[1]);
    return 1;
}

/* GET <filename> -> "OK FILE_SEND <filename> <filesize>" + exactly
 * <filesize> raw bytes. Returns 0 if the connection broke, else 1. */
static int cmd_get(session_t *s, char *tokens[], int ntok)
{
    char           path[256];
    char           chunk[FILE_CHUNK];
    struct stat    st;
    FILE          *fp;
    long           sent = 0;
    size_t         n;
    struct timeval start;

    if (ntok != 2) {
        send_error(s, E_BAD_ARGS);
        return 1;
    }
    if (!valid_filename(tokens[1])) {
        send_error(s, E_BAD_FILENAME);
        return 1;
    }

    snprintf(path, sizeof(path), "%s/%s", STORAGE_DIR, tokens[1]);
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
        (fp = fopen(path, "rb")) == NULL) {
        log_event("Client %d GET %s - file not found", s->client_id, tokens[1]);
        send_error(s, E_NOT_FOUND);
        return 1;
    }

    if (send_response(s, "OK FILE_SEND %s %ld", tokens[1], (long)st.st_size) < 0) {
        fclose(fp);
        return 0;
    }

    gettimeofday(&start, NULL);
    while (sent < (long)st.st_size && (n = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        if ((long)n > st.st_size - sent)          /* never send more than announced */
            n = st.st_size - sent;
        if (send_all(s->sock, chunk, n) < 0) {
            fclose(fp);
            log_event("Client %d GET %s aborted - connection lost", s->client_id, tokens[1]);
            return 0;
        }
        sent += n;
    }
    fclose(fp);
    double secs = seconds_since(&start);

    if (sent != (long)st.st_size) {               /* file shrank while reading */
        log_event("Client %d GET %s short read - closing connection", s->client_id, tokens[1]);
        return 0;
    }
    log_event("Client %d GET %s sent (%ld bytes in %.3f s, %.0f bytes/s)",
              s->client_id, tokens[1], sent, secs, secs > 0 ? sent / secs : (double)sent);
    return 1;
}

/* ================================================================== */
/*                   Per-connection thread (session)                  */
/* ================================================================== */

/* Decide what to do with one command line. Returns 1 to keep the
 * connection open, 0 to close it. */
static int handle_command(session_t *s, char *line)
{
    char  copy[MAX_LINE];
    char *tokens[MAX_TOKENS];
    int   ntok;

    /* never write the auth token into the log file */
    if (strncmp(line, "AUTH ", 5) == 0)
        log_event("Client %d -> AUTH ********", s->client_id);
    else
        log_event("Client %d -> %s", s->client_id, line);

    snprintf(copy, sizeof(copy), "%s", line);
    ntok = split_tokens(copy, tokens, MAX_TOKENS);
    if (ntok == 0) {
        send_error(s, E_UNKNOWN);
        return 1;
    }

    /* AUTH must come first. PUT is let through so its file bytes can be
     * discarded (cmd_put sends NOT_AUTHENTICATED itself). */
    if (strcmp(tokens[0], "AUTH") == 0)
        return cmd_auth(s, tokens, ntok);
    if (strcmp(tokens[0], "PUT") == 0)
        return cmd_put(s, tokens, ntok);
    if (!s->authenticated) {
        send_error(s, E_NOT_AUTH);
        return 1;
    }

    if (ntok > MAX_TOKENS) {
        send_error(s, E_BAD_ARGS);
    } else if (strcmp(tokens[0], "SYSINFO") == 0) {
        if (ntok != 1) send_error(s, E_BAD_ARGS); else cmd_sysinfo(s);
    } else if (strcmp(tokens[0], "LISTPROC") == 0) {
        if (ntok != 1) send_error(s, E_BAD_ARGS); else cmd_listproc(s);
    } else if (strcmp(tokens[0], "EXEC") == 0) {
        cmd_exec(s, tokens, ntok);
    } else if (strcmp(tokens[0], "GET") == 0) {
        return cmd_get(s, tokens, ntok);
    } else if (strcmp(tokens[0], "MONITOR") == 0) {
        cmd_monitor(s, tokens, ntok);
    } else if (strcmp(tokens[0], "QUIT") == 0) {
        if (ntok != 1) {
            send_error(s, E_BAD_ARGS);
            return 1;
        }
        stop_monitor(s);                          /* stop UDP stream first */
        send_response(s, "OK BYE");
        s->quit_received = 1;
        return 0;
    } else {
        send_error(s, E_UNKNOWN);
    }
    return 1;
}

static void *client_thread(void *arg)
{
    session_t *s = (session_t *)arg;
    char       line[MAX_LINE];
    int        keep_open = 1;

    log_event("Client %d connected from %s (thread handling it)", s->client_id, s->peer);

    while (keep_open) {
        int n = read_line(&s->reader, line, sizeof(line));

        if (n == -1) {                    /* recv() returned 0 or error */
            break;
        }
        if (n == -2) {
            send_error(s, E_LINE_TOO_LONG);
            break;                        /* cannot find the next line start */
        }
        keep_open = handle_command(s, line);
    }

    stop_monitor(s);                      /* also covers ungraceful exits */
    close(s->sock);
    log_event("Client %d disconnected (%s)", s->client_id,
              s->quit_received ? "QUIT - graceful" : "connection closed by peer/agent");

    pthread_mutex_destroy(&s->monitor_lock);
    free(s);
    return NULL;
}

/* ================================================================== */
/*                               main                                 */
/* ================================================================== */

int main(void)
{
    int                server_fd;
    int                opt = 1;
    int                next_client_id = 1;
    struct sockaddr_in server_addr;

    /* Writing to a socket whose peer has gone raises SIGPIPE, which would
     * kill the whole Agent. Ignore it; send() then just returns -1. */
    signal(SIGPIPE, SIG_IGN);

    /* Personalised storage directory ./agentfiles/IT24102235 */
    mkdir(STORAGE_ROOT, 0755);
    mkdir(STORAGE_DIR, 0755);

    log_fp = fopen(LOG_FILE, "a");
    if (log_fp == NULL) {
        perror("Cannot open log file");
        exit(EXIT_FAILURE);
    }

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Socket failed");
        exit(EXIT_FAILURE);
    }
    /* allow quick restart of the Agent on the same port */
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons(AGENT_PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    if (listen(server_fd, BACKLOG) < 0) {
        perror("Listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    log_event("RemoteOps Agent started (PID %d) for %s", getpid(), REG_NUMBER);
    log_event("Listening on TCP port %d | tag %s | log %s | storage %s/",
              AGENT_PORT, SID_TAG, LOG_FILE, STORAGE_DIR);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t          addr_len = sizeof(client_addr);
        pthread_t          tid;

        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno != EINTR)
                perror("Accept failed");
            continue;
        }

        session_t *s = calloc(1, sizeof(session_t));
        if (s == NULL) {
            close(client_fd);
            continue;
        }
        s->client_id = next_client_id++;
        s->sock      = client_fd;
        s->addr      = client_addr;
        s->udp_sock  = -1;
        s->reader.fd = client_fd;
        pthread_mutex_init(&s->monitor_lock, NULL);
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        snprintf(s->peer, sizeof(s->peer), "%s:%d", ip, ntohs(client_addr.sin_port));

        /* one thread per Controller; detached so it cleans up itself */
        if (pthread_create(&tid, NULL, client_thread, s) != 0) {
            perror("pthread_create failed");
            close(client_fd);
            pthread_mutex_destroy(&s->monitor_lock);
            free(s);
            continue;
        }
        pthread_detach(tid);
    }

    close(server_fd);
    fclose(log_fp);
    return 0;
}
