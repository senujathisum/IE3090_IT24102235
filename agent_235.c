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

typedef struct {
    int  fd;
    char buf[READ_BUF_SIZE];
    int  len;
} conn_reader;

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

static int send_response_raw(int sock, int client_id, const char *fmt, ...)
{
    char    line[MAX_RESPONSE + 32];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(line, MAX_RESPONSE, fmt, ap);
    va_end(ap);
    if (len >= MAX_RESPONSE) len = MAX_RESPONSE - 1;

    len += snprintf(line + len, sizeof(line) - len, " %s\n", SID_TAG);
    log_event("Client %d <- %.*s", client_id, len - 1, line);
    return send_all(sock, line, len);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    mkdir(STORAGE_ROOT, 0755);
    mkdir(STORAGE_DIR, 0755);
    log_fp = fopen(LOG_FILE, "a");
    log_event("Framing logic verified.");
    if (log_fp) fclose(log_fp);
    return 0;
}