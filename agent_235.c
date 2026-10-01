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

int main(void)
{
    int server_fd;
    int opt = 1;
    struct sockaddr_in server_addr;

    signal(SIGPIPE, SIG_IGN);
    mkdir(STORAGE_ROOT, 0755);
    mkdir(STORAGE_DIR, 0755);

    log_fp = fopen(LOG_FILE, "a");
    if (!log_fp) {
        perror("Cannot open log file");
        exit(EXIT_FAILURE);
    }

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Socket failed");
        exit(EXIT_FAILURE);
    }
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
    log_event("Listening on TCP port %d | tag %s | log %s", AGENT_PORT, SID_TAG, LOG_FILE);

    close(server_fd);
    fclose(log_fp);
    return 0;
}