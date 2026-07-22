# xinit: parallel init system for FreeBSD

> A custom PID 1 + service manager written in C, with parallel service
> startup, dependency ordering, per-service logging, and a live control
> socket.

---

## Architecture

```
kernel
  └── xinit  (PID 1) - reaps zombies, handles reboot/poweroff
        └── xinictl  (daemon) - loads units, spawns service threads
               ├── [thread] syslog   ──────────────── starts immediately
               ├── [thread] network  ──────────────── starts immediately
               ├── [thread] sshd     ── waits for: network, syslog
               └── [thread] cron     ── waits for: syslog
```

Parallel startup: every service gets its own pthread. Services with
`After = ...` dependencies **spin-poll** until their deps are RUNNING,
then launch. No dependency graph pre-computation needed — the threads
just wait.

---

## Build

```sh
# FreeBSD
make

# Linux (for testing, needs -DTEST_NO_REBOOT tweak)
make CC=gcc
```

Produces two binaries: `xinit` and `xinictl`.

---

## Install

```sh
sudo make install           # installs to /usr/local/sbin/
sudo make install-examples  # copies sample .service files to /etc/xinit/services/
```

Then tell the FreeBSD bootloader to use xinit as PID 1.
Add to `/boot/loader.conf`:

```
init_path="/sbin/xinit"
```

Or for testing without replacing your real init, boot with:

```
# at the loader prompt:
set init_path=/usr/local/sbin/xinit
boot
```

---

## Service unit files

Place files ending in `.service` in `/etc/xinit/services/`.

### All keys

| Key            | Required | Default | Description                              |
|----------------|----------|---------|------------------------------------------|
| `Name`         | yes      | —       | Internal service name                    |
| `Description`  | no       | —       | Human-readable label                     |
| `ExecStart`    | yes      | —       | Command to run (passed to `/bin/sh -c`)  |
| `ExecStop`     | no       | —       | Command to stop the service              |
| `ExecPre`      | no       | —       | Command run before ExecStart             |
| `After`        | no       | —       | Comma-separated dependency names         |
| `Restart`      | no       | 0       | 1 = restart on any exit                  |
| `RestartDelay` | no       | 3       | Seconds between restarts                 |
| `TimeoutStart` | no       | 30      | Startup timeout (informational for now)  |
| `OneShot`      | no       | 0       | 1 = run once, don't restart              |
| `Enabled`      | no       | 1       | 0 = skip at boot                         |
| `Environment`  | no       | —       | `KEY=value`, one per line                |

### Example

```ini
Name        = myapp
Description = My cool daemon
After       = network, syslog
ExecPre     = /usr/local/bin/myapp-migrate
ExecStart   = /usr/local/bin/myapp --config /etc/myapp.conf
Restart     = 1
RestartDelay= 5
Environment = APP_ENV=production
Environment = PORT=8080
Enabled     = 1
```

---

## xinictl CLI

Once the system is running, control services from the shell:

```sh
xinictl list              # show all services and their states
xinictl status            # same
xinictl start sshd        # start a stopped service
xinictl stop  sshd        # stop a running service
xinictl restart sshd      # restart
xinictl reboot            # reboot the system
xinictl poweroff          # power off
```

---

## Logging

Every service gets its own log file:

```
/var/log/xinit/xinictl.log   ← service manager itself
/var/log/xinit/syslog.log    ← syslogd stdout/stderr
/var/log/xinit/sshd.log      ← sshd stdout/stderr
...
```

xinit itself logs to `/var/log/xinit.log`.

---

## Signals to xinit (PID 1)

| Signal    | Effect                  |
|-----------|-------------------------|
| `SIGTERM` | Graceful shutdown       |
| `SIGUSR1` | Reboot                  |
| `SIGUSR2` | Power off               |
| `SIGHUP`  | Forward HUP to xinictl  |

---

## Project structure

```
xinit/
├── src/
│   ├── xinit.c       ← PID 1 init
│   └── xinictl.c     ← service manager + CLI
├── services/
│   ├── syslog.service
│   ├── network.service
│   ├── sshd.service
│   └── cron.service
├── man/
│   ├── xinit.8
│   └── xinictl.8
├── Makefile
└── README.md
```

---

## License

MIT License
