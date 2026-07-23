/*
 * xinictl - service manager for xinit
 *
 * Features:
 *   - Parallel service startup with dependency resolution
 *   - Per-service logging  (/var/log/xinit/<svc>.log)
 *   - Socket-based control (xinictl start|stop|status|list)
 *   - Service unit files in /etc/xinit/services/
 *
 * Build: cc -O2 -pthread -o xinictl src/xinictl.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

/* constants  */

#define MAX_SERVICES   128
#define MAX_DEPS       16
#define MAX_NAME       64
#define MAX_CMD        512
#define MAX_DESC       256
#define MAX_ENV        32

#define SVC_DIR        "/etc/xinit/services"
#define LOG_DIR        "/var/log/xinit"
#define RUN_DIR        "/run/xinit"
#define SOCKET_PATH    "/run/xinit/xinictl.sock"
#define VERSION        "1.0.0"

/*  service state  */

typedef enum {
    SVC_DISABLED  = 0,
    SVC_PENDING,      /* waiting for deps */
    SVC_STARTING,
    SVC_RUNNING,
    SVC_STOPPING,
    SVC_STOPPED,
    SVC_FAILED,
} SvcState;

static const char *state_str[] = {
    "disabled", "pending", "starting",
    "running",  "stopping","stopped", "failed"
};

static const char *state_color[] = {
    "\033[0;90m",     /* disabled - gray    */
    "\033[0;33m",     /* pending  - yellow  */
    "\033[0;34m",     /* starting - blue    */
    "\033[0;32m",     /* running  - green   */
    "\033[0;33m",     /* stopping - yellow  */
    "\033[0;90m",     /* stopped  - gray    */
    "\033[0;31m",     /* failed   - red     */
};

#define RESET "\033[0m"

typedef struct {
    char     name[MAX_NAME];
    char     description[MAX_DESC];
    char     exec_start[MAX_CMD];
    char     exec_stop[MAX_CMD];    /* optional */
    char     exec_pre[MAX_CMD];     /* optional pre-start hook */
    char     deps[MAX_DEPS][MAX_NAME];
    int      ndeps;
    char     env[MAX_ENV][MAX_CMD];
    int      nenv;

    int      restart;        /* 1 = always restart on failure */
    int      restart_delay;  /* seconds between restarts      */
    int      timeout_start;  /* seconds to wait for "running" */
    int      oneshot;        /* 1 = run-once, don't restart   */
    int      enabled;

    /* runtime */
    SvcState  state;
    pid_t     pid;
    time_t    started_at;
    int       restart_count;
    pthread_t thread;
    pthread_mutex_t lock;
} Service;

/* globals  */

static Service   svcs[MAX_SERVICES];
static int       nsvc = 0;
static pthread_mutex_t gslock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_quit = 0;

/* logging  */

static void xlog(const char *svc, const char *level, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void xlog(const char *svc, const char *level, const char *fmt, ...)
{
    /* per-service log file */
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, svc ? svc : "xinictl");

    FILE *f = fopen(path, "a");

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tm);

    va_list ap;
    va_start(ap, fmt);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (f) {
        fprintf(f, "[%s] [%s] %s\n", tbuf, level, msg);
        fclose(f);
    }

    /* always echo to console too */
    const char *col = strcmp(level,"ERROR")==0 ? "\033[0;31m" :
                      strcmp(level,"WARN") ==0 ? "\033[0;33m" :
                      strcmp(level,"OK")   ==0 ? "\033[0;32m" :
                                                 "\033[0;37m";
    printf("\033[1;35m[xinictl]\033[0m %s[%-7s]" RESET
           " \033[1m%-20s\033[0m %s\n",
           col, level, svc ? svc : "xinictl", msg);
    fflush(stdout);
}

/* unit file parser */

static void svc_init(Service *s)
{
    memset(s, 0, sizeof(*s));
    s->restart_delay = 3;
    s->timeout_start = 30;
    s->enabled = 1;
    pthread_mutex_init(&s->lock, NULL);
}

/* trim leading/trailing whitespace in-place */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s) - 1;
    while (e > s && (*e == ' ' || *e == '\t' || *e == '\n' || *e == '\r'))
        *e-- = '\0';
    return s;
}

static int parse_unit(const char *path, Service *s)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (*p == '#' || *p == '\0') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(p);
        char *val = trim(eq + 1);

#define MATCH(K) (strcmp(key, K) == 0)
        if      (MATCH("Name"))        strncpy(s->name,        val, MAX_NAME-1);
        else if (MATCH("Description")) strncpy(s->description, val, MAX_DESC-1);
        else if (MATCH("ExecStart"))   strncpy(s->exec_start,  val, MAX_CMD-1);
        else if (MATCH("ExecStop"))    strncpy(s->exec_stop,   val, MAX_CMD-1);
        else if (MATCH("ExecPre"))     strncpy(s->exec_pre,    val, MAX_CMD-1);
        else if (MATCH("Restart"))     s->restart = atoi(val);
        else if (MATCH("RestartDelay"))s->restart_delay = atoi(val);
        else if (MATCH("TimeoutStart"))s->timeout_start = atoi(val);
        else if (MATCH("OneShot"))     s->oneshot = atoi(val);
        else if (MATCH("Enabled"))     s->enabled = atoi(val);
        else if (MATCH("After")) {
            /* comma-separated dep list */
            char tmp[MAX_CMD];
            strncpy(tmp, val, MAX_CMD-1);
            char *tok = strtok(tmp, ",");
            while (tok && s->ndeps < MAX_DEPS) {
                strncpy(s->deps[s->ndeps++], trim(tok), MAX_NAME-1);
                tok = strtok(NULL, ",");
            }
        } else if (MATCH("Environment")) {
            if (s->nenv < MAX_ENV)
                strncpy(s->env[s->nenv++], val, MAX_CMD-1);
        }
#undef MATCH
    }
    fclose(f);
    return (s->name[0] && s->exec_start[0]) ? 0 : -1;
}

static void load_units(void)
{
    DIR *d = opendir(SVC_DIR);
    if (!d) {
        xlog(NULL, "WARN", "service dir %s not found", SVC_DIR);
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) && nsvc < MAX_SERVICES) {
        /* only *.service files */
        size_t nl = strlen(de->d_name);
        if (nl < 9 || strcmp(de->d_name + nl - 8, ".service") != 0)
            continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", SVC_DIR, de->d_name);

        Service *s = &svcs[nsvc];
        svc_init(s);
        if (parse_unit(path, s) == 0) {
            s->state = s->enabled ? SVC_PENDING : SVC_DISABLED;
            xlog(s->name, "INFO", "unit loaded (%s)", s->description);
            nsvc++;
        } else {
            xlog(NULL, "WARN", "invalid unit file: %s", path);
            pthread_mutex_destroy(&s->lock);
        }
    }
    closedir(d);
    xlog(NULL, "INFO", "loaded %d service unit(s)", nsvc);
}

/* dependency resolver */

static Service *find_svc(const char *name)
{
    for (int i = 0; i < nsvc; i++)
        if (strcmp(svcs[i].name, name) == 0)
            return &svcs[i];
    return NULL;
}

/* returns 1 if all deps are RUNNING (or STOPPED for oneshots) */
static int deps_satisfied(Service *s)
{
    for (int i = 0; i < s->ndeps; i++) {
        Service *dep = find_svc(s->deps[i]);
        if (!dep) continue; /* unknown dep – assume ok */
        SvcState ds;
        pthread_mutex_lock(&dep->lock);
        ds = dep->state;
        pthread_mutex_unlock(&dep->lock);
        if (ds != SVC_RUNNING && ds != SVC_STOPPED)
            return 0;
    }
    return 1;
}

/* service runner thread*/

static void svc_set_state(Service *s, SvcState st)
{
    pthread_mutex_lock(&s->lock);
    s->state = st;
    pthread_mutex_unlock(&s->lock);
}

static pid_t svc_exec(Service *s, const char *cmd)
{
    pid_t pid = fork();
    if (pid < 0) {
        xlog(s->name, "ERROR", "fork: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        /* child */
        setsid();

        /* redirect stdout/stderr to per-service log */
        char logpath[256];
        snprintf(logpath, sizeof(logpath), "%s/%s.log", LOG_DIR, s->name);
        int lfd = open(logpath, O_WRONLY|O_CREAT|O_APPEND, 0644);
        if (lfd >= 0) {
            dup2(lfd, STDOUT_FILENO);
            dup2(lfd, STDERR_FILENO);
            close(lfd);
        }

        /* apply environment */
        for (int i = 0; i < s->nenv; i++)
            putenv(s->env[i]);

        execl("/bin/sh", "sh", "-c", cmd, NULL);
        _exit(127);
    }
    return pid;
}

static void *svc_thread(void *arg)
{
    Service *s = (Service *)arg;

    /* wait for deps */
    while (!g_quit) {
        if (deps_satisfied(s)) break;
        usleep(200000); /* 200 ms poll */
    }
    if (g_quit) return NULL;

    do {
        /* pre-start hook */
        if (s->exec_pre[0]) {
            xlog(s->name, "INFO", "running ExecPre");
            pid_t pre = svc_exec(s, s->exec_pre);
            if (pre > 0) waitpid(pre, NULL, 0);
        }

        svc_set_state(s, SVC_STARTING);
        xlog(s->name, "INFO", "starting: %s", s->exec_start);

        pid_t pid = svc_exec(s, s->exec_start);
        if (pid < 0) {
            svc_set_state(s, SVC_FAILED);
            xlog(s->name, "ERROR", "failed to spawn process");
            break;
        }

        pthread_mutex_lock(&s->lock);
        s->pid        = pid;
        s->started_at = time(NULL);
        s->state      = SVC_RUNNING;
        pthread_mutex_unlock(&s->lock);

        xlog(s->name, "OK", "running (pid %d)", pid);

        /* write pidfile */
        char pf[128];
        snprintf(pf, sizeof(pf), "%s/%s.pid", RUN_DIR, s->name);
        FILE *pff = fopen(pf, "w");
        if (pff) { fprintf(pff, "%d\n", pid); fclose(pff); }

        int status;
        waitpid(pid, &status, 0);

        pthread_mutex_lock(&s->lock);
        s->pid = -1;
        pthread_mutex_unlock(&s->lock);

        if (g_quit) {
            svc_set_state(s, SVC_STOPPED);
            return NULL;
        }

        int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

        if (s->oneshot) {
            svc_set_state(s, exit_code == 0 ? SVC_STOPPED : SVC_FAILED);
            xlog(s->name, exit_code == 0 ? "OK" : "ERROR",
                 "oneshot exited (code %d)", exit_code);
            return NULL;
        }

        if (exit_code == 0 && !s->restart) {
            svc_set_state(s, SVC_STOPPED);
            xlog(s->name, "INFO", "exited cleanly");
            return NULL;
        }

        s->restart_count++;
        svc_set_state(s, SVC_FAILED);
        xlog(s->name, "WARN",
             "exited (code %d), restart #%d in %ds",
             exit_code, s->restart_count, s->restart_delay);
        sleep(s->restart_delay);

    } while (s->restart && !g_quit);

    return NULL;
}

/*  parallel launcher  */

static void start_all(void)
{
    xlog(NULL, "INFO", "parallel service startup begin");
    for (int i = 0; i < nsvc; i++) {
        Service *s = &svcs[i];
        if (!s->enabled || s->state == SVC_DISABLED) continue;
        pthread_create(&s->thread, NULL, svc_thread, s);
    }
    xlog(NULL, "INFO", "all service threads spawned");
}

static void stop_all(void)
{
    xlog(NULL, "INFO", "stopping all services");
    g_quit = 1;

    /* send SIGTERM to all running services */
    for (int i = 0; i < nsvc; i++) {
        Service *s = &svcs[i];
        pthread_mutex_lock(&s->lock);
        if (s->state == SVC_RUNNING && s->pid > 0) {
            xlog(s->name, "INFO", "sending SIGTERM (pid %d)", s->pid);
            kill(s->pid, SIGTERM);
            s->state = SVC_STOPPING;
        }
        pthread_mutex_unlock(&s->lock);
    }

    /* wait for threads */
    for (int i = 0; i < nsvc; i++) {
        Service *s = &svcs[i];
        if (s->enabled)
            pthread_join(s->thread, NULL);
    }
    xlog(NULL, "INFO", "all services stopped");
}

/* control socket  */

static void handle_cmd(int fd, const char *cmd)
{
    char resp[4096];
    resp[0] = '\0';

    if (strcmp(cmd, "list") == 0 || strcmp(cmd, "status") == 0) {
        snprintf(resp, sizeof(resp),
            "%-20s %-10s %-8s %s\n"
            "%-20s %-10s %-8s %s\n",
            "SERVICE", "STATE", "PID", "RESTARTS",
            "-------", "-----", "---", "--------");

        for (int i = 0; i < nsvc; i++) {
            Service *s = &svcs[i];
            pthread_mutex_lock(&s->lock);
            char line[256];
            snprintf(line, sizeof(line), "%-20s %-10s %-8d %d\n",
                     s->name, state_str[s->state],
                     (int)s->pid, s->restart_count);
            strncat(resp, line, sizeof(resp) - strlen(resp) - 1);
            pthread_mutex_unlock(&s->lock);
        }

    } else if (strncmp(cmd, "start ", 6) == 0) {
        const char *name = cmd + 6;
        Service *s = find_svc(name);
        if (!s) {
            snprintf(resp, sizeof(resp), "ERROR: service '%s' not found\n", name);
        } else {
            pthread_mutex_lock(&s->lock);
            SvcState st = s->state;
            pthread_mutex_unlock(&s->lock);
            if (st == SVC_RUNNING) {
                snprintf(resp, sizeof(resp), "INFO: '%s' already running\n", name);
            } else {
                s->state = SVC_PENDING;
                pthread_create(&s->thread, NULL, svc_thread, s);
                snprintf(resp, sizeof(resp), "OK: starting '%s'\n", name);
            }
        }

    } else if (strncmp(cmd, "stop ", 5) == 0) {
        const char *name = cmd + 5;
        Service *s = find_svc(name);
        if (!s) {
            snprintf(resp, sizeof(resp), "ERROR: service '%s' not found\n", name);
        } else {
            pthread_mutex_lock(&s->lock);
            if (s->state == SVC_RUNNING && s->pid > 0) {
                kill(s->pid, SIGTERM);
                s->state = SVC_STOPPING;
                snprintf(resp, sizeof(resp), "OK: stopping '%s' (pid %d)\n",
                         name, (int)s->pid);
            } else {
                snprintf(resp, sizeof(resp), "INFO: '%s' not running\n", name);
            }
            pthread_mutex_unlock(&s->lock);
        }

    } else if (strncmp(cmd, "restart ", 8) == 0) {
        const char *name = cmd + 8;
        Service *s = find_svc(name);
        if (!s) {
            snprintf(resp, sizeof(resp), "ERROR: not found\n");
        } else {
            pthread_mutex_lock(&s->lock);
            if (s->pid > 0) kill(s->pid, SIGTERM);
            pthread_mutex_unlock(&s->lock);
            snprintf(resp, sizeof(resp), "OK: restarting '%s'\n", name);
        }

    } else if (strcmp(cmd, "reload") == 0) {
        /* TODO: hot-reload unit files */
        snprintf(resp, sizeof(resp), "OK: reload not yet implemented\n");

    } else if (strcmp(cmd, "poweroff") == 0) {
        snprintf(resp, sizeof(resp), "OK: poweroff\n");
        write(fd, resp, strlen(resp));
        kill(getppid(), SIGUSR2);
        return;

    } else if (strcmp(cmd, "reboot") == 0) {
        snprintf(resp, sizeof(resp), "OK: rebooting\n");
        write(fd, resp, strlen(resp));
        kill(getppid(), SIGUSR1);
        return;

    } else {
        snprintf(resp, sizeof(resp),
                 "commands: list status start <svc> stop <svc> "
                 "restart <svc> reload poweroff reboot\n");
    }

    write(fd, resp, strlen(resp));
}

static void *socket_thread(void *arg)
{
    (void)arg;
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { xlog(NULL,"ERROR","socket: %s", strerror(errno)); return NULL; }

    unlink(SOCKET_PATH);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path)-1);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(srv, 8) < 0) {
        xlog(NULL,"ERROR","bind/listen: %s", strerror(errno));
        close(srv);
        return NULL;
    }
    chmod(SOCKET_PATH, 0660);
    xlog(NULL,"INFO","control socket at %s", SOCKET_PATH);

    while (!g_quit) {
        int cl = accept(srv, NULL, NULL);
        if (cl < 0) continue;

        char buf[256];
        ssize_t n = read(cl, buf, sizeof(buf)-1);
        if (n > 0) {
            buf[n] = '\0';
            buf[strcspn(buf, "\n\r")] = '\0';
            handle_cmd(cl, buf);
        }
        close(cl);
    }
    close(srv);
    unlink(SOCKET_PATH);
    return NULL;
}

/* signal handling  */

static void sig_handler(int sig)
{
    if (sig == SIGTERM || sig == SIGINT) {
        xlog(NULL, "INFO", "shutdown signal received");
        g_quit = 1;
    } else if (sig == SIGHUP) {
        xlog(NULL, "INFO", "SIGHUP – would reload units");
    }
}

/*  main */

int main(int argc, char *argv[])
{
    /* CLI mode: xinictl <command> [args] */
    if (argc >= 2 && strcmp(argv[1], "--daemon") != 0) {
        /* send command via socket */
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) { perror("socket"); return 1; }

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path)-1);

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            fprintf(stderr, "xinictl: cannot connect to daemon "
                            "(%s not running?)\n", SOCKET_PATH);
            close(fd); return 1;
        }

        /* build command string from argv */
        char cmd[512] = "";
        for (int i = 1; i < argc; i++) {
            strncat(cmd, argv[i], sizeof(cmd)-strlen(cmd)-2);
            if (i < argc-1) strncat(cmd, " ", sizeof(cmd)-strlen(cmd)-1);
        }
        write(fd, cmd, strlen(cmd));

        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf)-1)) > 0) {
            buf[n] = '\0';
            fputs(buf, stdout);
        }
        close(fd);
        return 0;
    }

    /* Daemon mode */
    mkdir(LOG_DIR, 0755);
    mkdir(RUN_DIR, 0755);

    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);
    signal(SIGHUP,  sig_handler);
    signal(SIGCHLD, SIG_DFL);   /* let waitpid work */

    xlog(NULL, "INFO", "xinictl v%s starting", VERSION);

    load_units();

    pthread_t sock_th;
    pthread_create(&sock_th, NULL, socket_thread, NULL);

    start_all();

    /* main thread waits until shutdown */
    while (!g_quit) sleep(1);

    stop_all();
    pthread_join(sock_th, NULL);

    xlog(NULL, "INFO", "xinictl exiting");
    return 0;
}
mily = AF_UNIX;
        strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path)-1);

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            fprintf(stderr, "xinictl: cannot connect to daemon "
                            "(%s not running?)\n", SOCKET_PATH);
            close(fd); return 1;
        }

        /* build command string from argv */
        char cmd[512] = "";
        for (int i = 1; i < argc; i++) {
            strncat(cmd, argv[i], sizeof(cmd)-strlen(cmd)-2);
            if (i < argc-1) strncat(cmd, " ", sizeof(cmd)-strlen(cmd)-1);
        }
        write(fd, cmd, strlen(cmd));

        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf)-1)) > 0) {
            buf[n] = '\0';
            fputs(buf, stdout);
        }
        close(fd);
        return 0;
    }

    /* Daemon mode */
    mkdir(LOG_DIR, 0755);
    mkdir(RUN_DIR, 0755);

    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);
    signal(SIGHUP,  sig_handler);
    signal(SIGCHLD, SIG_DFL);   /* let waitpid work */

    xlog(NULL, "INFO", "xinictl v%s starting", VERSION);

    load_units();

    pthread_t sock_th;
    pthread_create(&sock_th, NULL, socket_thread, NULL);

    start_all();

    /* main thread waits until shutdown */
    while (!g_quit) sleep(1);

    stop_all();
    pthread_join(sock_th, NULL);

    xlog(NULL, "INFO", "xinictl exiting");
    return 0;
}
