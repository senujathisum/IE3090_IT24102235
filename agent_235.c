#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define REG_NUMBER      "IT24102235"
#define AGENT_PORT      9410
#define SID_TAG         "SID:5322"
#define AUTH_TOKEN      "OPS-2235"
#define LOG_FILE        "remoteops_IT24102235.log"
#define STORAGE_ROOT    "./agentfiles"
#define STORAGE_DIR     "./agentfiles/IT24102235"
#define BACKLOG         10
#define MAX_LINE        1024
#define READ_BUF_SIZE   4096
#define MAX_RESPONSE    65536
#define MAX_TOKENS      4
#define MAX_AUTH_ATTEMPTS 3

#define E_AUTH_FAILED   "001 AUTH_FAILED"
#define E_NOT_ALLOWED   "002 COMMAND_NOT_ALLOWED"
#define E_NOT_AUTH      "003 NOT_AUTHENTICATED"
#define E_UNKNOWN       "006 UNKNOWN_COMMAND"
#define E_INTERNAL      "011 INTERNAL_ERROR"
#define E_BAD_ARGS      "007 INVALID_ARGUMENTS"

typedef struct {
    int  fd;
    char buf[READ_BUF_SIZE];
    int  len;
} conn_reader;

typedef struct {
    int                client_id;
    int                sock;
    struct sockaddr_in addr;
    char               peer[64];
    conn_reader        reader;
    int                authenticated;
    int                auth_attempts;
} session_t;

static FILE           *log_fp = NULL;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

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

    localtime_r(&now, &tm_now);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_now);

    pthread_mutex_lock(&log_lock);
    if (log_fp != NULL) {
        fprintf(log_fp, "[%s] %s\n", stamp, msg);
        fflush(log_fp);
    }
    printf("[%s] %s\n", stamp, msg);
    fflush(stdout);
    pthread_mutex_unlock(&log_lock);
}

static int send_all(int sock, const char *data, long len)
{
    long total = 0;
    while (total < len) {
        ssize_t n = send(sock, data + total, len - total, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += n;
    }
    return 0;
}

static int read_line(conn_reader *r, char *line, int max)
{
    while (1) {
        char *nl = memchr(r->buf, '\n', r->len);
        if (nl != NULL) {
            int line_len = (int)(nl - r->buf);
            int used     = line_len + 1;
            if (line_len >= max) return -2;
            memcpy(line, r->buf, line_len);
            if (line_len > 0 && line[line_len - 1] == '\r') line_len--;
            line[line_len] = '\0';
            memmove(r->buf, r->buf + used, r->len - used);
            r->len -= used;
            return line_len;
        }
        if (r->len >= max || r->len == (int)sizeof(r->buf)) return -2;
        ssize_t n = recv(r->fd, r->buf + r->len, sizeof(r->buf) - r->len, 0);
        if (n == 0) return -1;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        r->len += (int)n;
    }
}

static int send_response(session_t *s, const char *fmt, ...)
{
    char    line[MAX_RESPONSE + 32];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(line, MAX_RESPONSE, fmt, ap);
    va_end(ap);
    if (len >= MAX_RESPONSE) len = MAX_RESPONSE - 1;
    len += snprintf(line + len, sizeof(line) - len, " %s\n", SID_TAG);
    log_event("Client %d <- %.*s", s->client_id, len - 1, line);
    return send_all(s->sock, line, len);
}

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
        if (fscanf(fp, "%lf", cpu_load) != 1) *cpu_load = 0.0;
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
        if (fscanf(fp, "%lf", &up) == 1) *uptime_sec = (long)up;
        fclose(fp);
    }
}

static void cmd_sysinfo(session_t *s)
{
    double cpu;
    long   mem, up;
    get_sysinfo(&cpu, &mem, &up);
    send_response(s, "OK SYSINFO %.2f %ld %ld", cpu, mem, up);
}

static void cmd_listproc(session_t *s)
{
    char  list[MAX_RESPONSE - 256];
    char  line[512];
    int   used = 0, count = 0;
    FILE *fp = popen("ps -e -o pid=,comm=", "r");
    if (!fp) {
        send_response(s, "ERR %s", E_INTERNAL);
        return;
    }

    list[0] = '\0';
    while (fgets(line, sizeof(line), fp) != NULL) {
        int  pid;
        char name[256];
        line[strcspn(line, "\n")] = '\0';
        if (sscanf(line, "%d %255[^\n]", &pid, name) != 2) continue;
        for (char *p = name; *p; p++) if (*p == ',') *p = '_';
        int need = snprintf(NULL, 0, "%s%d:%s", count ? "," : "", pid, name);
        if (used + need >= (int)sizeof(list) - 4) {
            strcat(list, ",...");
            break;
        }
        used += sprintf(list + used, "%s%d:%s", count ? "," : "", pid, name);
        count++;
    }
    pclose(fp);
    send_response(s, "OK PROCS %s", list);
}

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
    char output[4096] = {0}, line[512];
    int used = 0;
    if (ntok != 2) {
        send_response(s, "ERR %s", E_BAD_ARGS);
        return;
    }
    const char *command = whitelist_lookup(tokens[1]);
    if (!command) {
        send_response(s, "ERR %s", E_NOT_ALLOWED);
        return;
    }
    FILE *fp = popen(command, "r");
    if (!fp) {
        send_response(s, "ERR %s", E_INTERNAL);
        return;
    }
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\n")] = '\0';
        if (line[0] == '\0') continue;
        used += snprintf(output + used, sizeof(output) - used, "%s%s", used ? " | " : "", line);
        if (used >= (int)sizeof(output) - 1) break;
    }
    pclose(fp);
    send_response(s, "OK EXEC_RESULT %s", output[0] ? output : "(no output)");
}

static int split_tokens(char *line, char *tokens[], int max_tokens)
{
    char *save = NULL;
    int   count = 0;
    char *tok = strtok_r(line, " \t", &save);
    while (tok != NULL) {
        if (count == max_tokens) return max_tokens + 1;
        tokens[count++] = tok;
        tok = strtok_r(NULL, " \t", &save);
    }
    return count;
}

static int handle_command(session_t *s, char *line)
{
    char  copy[MAX_LINE];
    char *tokens[MAX_TOKENS];
    int   ntok;

    if (strncmp(line, "AUTH ", 5) == 0) log_event("Client %d -> AUTH ********", s->client_id);
    else log_event("Client %d -> %s", s->client_id, line);

    snprintf(copy, sizeof(copy), "%s", line);
    ntok = split_tokens(copy, tokens, MAX_TOKENS);
    if (ntok == 0) return 1;

    if (strcmp(tokens[0], "AUTH") == 0) {
        if (ntok == 2 && strcmp(tokens[1], AUTH_TOKEN) == 0) {
            s->authenticated = 1;
            log_event("Client %d AUTH success", s->client_id);
            send_response(s, "OK AUTHENTICATED");
            return 1;
        }
        s->auth_attempts++;
        send_response(s, "ERR %s", E_AUTH_FAILED);
        return (s->auth_attempts < MAX_AUTH_ATTEMPTS);
    }

    if (!s->authenticated) {
        send_response(s, "ERR %s", E_NOT_AUTH);
        return 1;
    }

    if (strcmp(tokens[0], "SYSINFO") == 0) {
        if (ntok != 1) send_response(s, "ERR %s", E_BAD_ARGS);
        else cmd_sysinfo(s);
    } else if (strcmp(tokens[0], "LISTPROC") == 0) {
        if (ntok != 1) send_response(s, "ERR %s", E_BAD_ARGS);
        else cmd_listproc(s);
    } else if (strcmp(tokens[0], "EXEC") == 0) {
        cmd_exec(s, tokens, ntok);
    } else if (strcmp(tokens[0], "QUIT") == 0) {
        send_response(s, "OK BYE");
        return 0;
    } else {
        send_response(s, "ERR %s", E_UNKNOWN);
    }
    return 1;
}

static void *client_thread(void *arg)
{
    session_t *s = (session_t *)arg;
    char       line[MAX_LINE];
    int        keep_open = 1;

    log_event("Client %d connected from %s", s->client_id, s->peer);
    while (keep_open) {
        int n = read_line(&s->reader, line, sizeof(line));
        if (n <= 0) break;
        keep_open = handle_command(s, line);
    }
    close(s->sock);
    log_event("Client %d disconnected", s->client_id);
    free(s);
    return NULL;
}

int main(void)
{
    int server_fd, opt = 1, next_client_id = 1;
    struct sockaddr_in server_addr;

    signal(SIGPIPE, SIG_IGN);
    mkdir(STORAGE_ROOT, 0755);
    mkdir(STORAGE_DIR, 0755);

    log_fp = fopen(LOG_FILE, "a");
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons(AGENT_PORT);

    bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    listen(server_fd, BACKLOG);

    log_event("RemoteOps Agent ready on port %d", AGENT_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        pthread_t tid;

        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) continue;

        session_t *s = calloc(1, sizeof(session_t));
        s->client_id = next_client_id++;
        s->sock      = client_fd;
        s->addr      = client_addr;
        s->reader.fd = client_fd;
        inet_ntop(AF_INET, &client_addr.sin_addr, s->peer, sizeof(s->peer));

        pthread_create(&tid, NULL, client_thread, s);
        pthread_detach(tid);
    }
    return 0;
}
