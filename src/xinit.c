/*
 * xinit - PID 1 init system for FreeBSD
 * Main daemon: spawns xinictl service manager
 *
 * Build: cc -O2 -o xinit src/xinit.c -lpthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include <sys/stat.h>

#define XINICTL_PATH   "/sbin/xinictl"
#define LOG_PATH       "/var/log/xinit.log"
#define RUN_DIR        "/run/xinit"
#define VERSION        "1.0.0"

static volatile sig_atomic_t g_reboot   = 0;
static volatile sig_atomic_t g_poweroff = 0;
static volatile sig_atomic_t g_running  = 1;
static pid_t g_xinictl_pid = -1;

/* helpers*/

static FILE *logfile = NULL;

static void log_open(void)
{
    logfile = fopen(LOG_PATH, "a");
    if (!logfile) logfile = stderr;
}

static void xlog(const char *level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void xlog(const char *level, const char *fmt, ...)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", t);

    va_list ap;
    va_start(ap, fmt);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    fprintf(logfile ? logfile : stderr,
            "[%s] [xinit] [%s] %s\n", tbuf, level, msg);
    if (logfile && logfile != stderr)
        fflush(logfile);

    /* also print to console */
    printf("\033[1;36m[xinit]\033[0m \033[0;33m[%s]\033[0m %s\n", level, msg);
    fflush(stdout);
}

/* signal handling*/

static void sig_handler(int signo)
{
    switch (signo) {
    case SIGCHLD:
        /* reap in main loop */
        break;
    case SIGTERM:
        xlog("INFO", "SIGTERM received – shutting down");
        g_running = 0;
        break;
    case SIGUSR1:
        xlog("INFO", "SIGUSR1 received – reboot requested");
        g_reboot = 1;
        g_running = 0;
        break;
    case SIGUSR2:
        xlog("INFO", "SIGUSR2 received – poweroff requested");
        g_poweroff = 1;
        g_running = 0;
        break;
    case SIGHUP:
        xlog("INFO", "SIGHUP received – reloading xinictl");
        if (g_xinictl_pid > 0)
            kill(g_xinictl_pid, SIGHUP);
        break;
    }
}

static void setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    sigaction(SIGCHLD, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);

    /* ignore these */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
}

/* filesystem bootstrap  */

static void mount_pseudo_fs(void)
{
    /* In a real deployment these would be mount(2) calls.
       Here we just ensure dirs exist. */
    mkdir("/run",       0755);
    mkdir("/run/xinit", 0755);
    mkdir("/run/lock",  0755);
    mkdir("/var/log",   0755);
    xlog("INFO", "pseudo-filesystems ready");
}

static void set_hostname(void)
{
    FILE *f = fopen("/etc/hostname", "r");
    if (!f) return;
    char hn[256];
    if (fgets(hn, sizeof(hn), f)) {
        hn[strcspn(hn, "\n")] = '\0';
        sethostname(hn, strlen(hn));
        xlog("INFO", "hostname -> %s", hn);
    }
    fclose(f);
}

/* spawn xinictl  */

static pid_t spawn_xinictl(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        xlog("ERROR", "fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        /* child: become session leader */
        setsid();
        /* open /dev/console as stdio */
        int fd = open("/dev/console", O_RDWR);
        if (fd >= 0) {
            dup2(fd, STDIN_FILENO);
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > 2) close(fd);
        }
        execl(XINICTL_PATH, "xinictl", "--daemon", NULL);
        /* fallback: shell */
        execl("/bin/sh", "sh", NULL);
        _exit(127);
    }
    xlog("INFO", "xinictl spawned (pid %d)", pid);
    return pid;
}

/* zombie reaper */

static void reap_children(void)
{
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (pid == g_xinictl_pid) {
            if (g_running) {
                xlog("WARN", "xinictl died (pid %d) – respawning in 1s", pid);
                sleep(1);
                g_xinictl_pid = spawn_xinictl();
            } else {
                xlog("INFO", "xinictl exited cleanly");
            }
        } else {
            xlog("DEBUG", "reaped orphan pid %d", pid);
        }
    }
}

/* shutdown sequence*/

static void do_shutdown(int reboot_flag)
{
    xlog("INFO", "sending SIGTERM to xinictl (%d)", g_xinictl_pid);
    if (g_xinictl_pid > 0) {
        kill(g_xinictl_pid, SIGTERM);
        /* wait up to 10 s */
        for (int i = 0; i < 100; i++) {
            usleep(100000);
            if (waitpid(g_xinictl_pid, NULL, WNOHANG) == g_xinictl_pid)
                break;
        }
        kill(g_xinictl_pid, SIGKILL);
    }

    xlog("INFO", "syncing filesystems");
    sync();

    if (reboot_flag) {
        xlog("INFO", "REBOOTING");
        reboot(RB_AUTOBOOT);
    } else {
        xlog("INFO", "POWERING OFF");
        reboot(RB_POWEROFF);
    }
}

/* entry point */

int main(int argc, char *argv[])
{
    if (getpid() != 1 && !(argc > 1 && strcmp(argv[1], "--no-pid1-check") == 0)) {
        fprintf(stderr, "xinit: must run as PID 1 "
                        "(pass --no-pid1-check to override for testing)\n");
        return 1;
    }

    log_open();
    xlog("INFO", "xinit v%s starting", VERSION);

    setup_signals();
    mount_pseudo_fs();
    set_hostname();

    /* write our pid */
    {
        FILE *pf = fopen("/run/xinit/xinit.pid", "w");
        if (pf) { fprintf(pf, "%d\n", getpid()); fclose(pf); }
    }

    g_xinictl_pid = spawn_xinictl();

    xlog("INFO", "entering main loop");
    while (g_running) {
        pause();          /* sleep until signal */
        reap_children();
    }

    reap_children();      /* final sweep */
    do_shutdown(g_reboot);

    /* unreachable */
    return 0;
}
───────────────── */

int main(int argc, char *argv[])
{
    if (getpid() != 1 && !(argc > 1 && strcmp(argv[1], "--no-pid1-check") == 0)) {
        fprintf(stderr, "xinit: must run as PID 1 "
                        "(pass --no-pid1-check to override for testing)\n");
        return 1;
    }

    log_open();
    xlog("INFO", "xinit v%s starting", VERSION);

    setup_signals();
    mount_pseudo_fs();
    set_hostname();

    /* write our pid */
    {
        FILE *pf = fopen("/run/xinit/xinit.pid", "w");
        if (pf) { fprintf(pf, "%d\n", getpid()); fclose(pf); }
    }

    g_xinictl_pid = spawn_xinictl();

    xlog("INFO", "entering main loop");
    while (g_running) {
        pause();          /* sleep until signal */
        reap_children();
    }

    reap_children();      /* final sweep */
    do_shutdown(g_reboot);

    /* unreachable */
    return 0;
}
