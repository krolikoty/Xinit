#!/bin/bash
# install-xinit.sh - Xinit distribution installer
# Автоматическая установка Xinit для FreeBSD

set -e

# Цвета для вывода
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Функции для логирования
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_ok() {
    echo -e "${GREEN}[OK]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Проверка что запущено на FreeBSD
check_freebsd() {
    if [ "$(uname -s)" != "FreeBSD" ]; then
        log_error "Этот скрипт работает только на FreeBSD"
        exit 1
    fi
    log_ok "Запущено на FreeBSD $(uname -r)"
}

# Проверка root
check_root() {
    if [ "$EUID" -ne 0 ]; then
        log_error "Нужны права root!"
        exit 1
    fi
    log_ok "Запущено с правами root"
}

# Компиляция xinit и xinictl
compile_binaries() {
    log_info "Компилирую xinit и xinictl..."
    
    # Проверяем что исходники есть
    if [ ! -f "xinit.c" ] || [ ! -f "xinictl.c" ]; then
        log_error "xinit.c и xinictl.c не найдены в текущей директории"
        exit 1
    fi
    
    # Компилируем xinit
    log_info "Компилирую xinit..."
    cc -O2 -o xinit xinit.c -lpthread
    if [ $? -ne 0 ]; then
        log_error "Ошибка при компиляции xinit"
        exit 1
    fi
    log_ok "xinit скомпилирован"
    
    # Компилируем xinictl
    log_info "Компилирую xinictl..."
    cc -O2 -pthread -o xinictl xinictl.c
    if [ $? -ne 0 ]; then
        log_error "Ошибка при компиляции xinictl"
        exit 1
    fi
    log_ok "xinictl скомпилирован"
}

# Установка бинарников
install_binaries() {
    log_info "Устанавливаю binaries в /sbin/..."
    
    install -m 755 xinit /sbin/xinit
    install -m 755 xinictl /sbin/xinictl
    
    log_ok "Binaries установлены"
}

# Создание директорий
create_directories() {
    log_info "Создаю директории..."
    
    mkdir -p /etc/xinit/services
    mkdir -p /var/log/xinit
    mkdir -p /run/xinit
    
    chmod 755 /etc/xinit
    chmod 755 /etc/xinit/services
    chmod 755 /var/log/xinit
    chmod 755 /run/xinit
    
    log_ok "Директории созданы"
}

# Создание rc.d скрипта
create_rc_script() {
    log_info "Создаю /etc/rc.d/xinit..."
    
    cat > /etc/rc.d/xinit << 'EOF'
#!/bin/sh
# /etc/rc.d/xinit - Xinit service manager

. /etc/rc.subr

name="xinit"
rcvar="xinit_enable"
command="/sbin/xinictl"
pidfile="/run/xinit/xinictl.pid"

start_cmd="xinit_start"
stop_cmd="xinit_stop"
restart_cmd="xinit_restart"

xinit_start() {
    echo "Starting xinit service manager..."
    /sbin/xinit --daemon
}

xinit_stop() {
    echo "Stopping xinit..."
    /sbin/xinictl poweroff 2>/dev/null || true
}

xinit_restart() {
    xinit_stop
    sleep 1
    xinit_start
}

load_rc_config $name
run_rc_command "$1"
EOF
    
    chmod 755 /etc/rc.d/xinit
    log_ok "rc.d скрипт создан"
}

# Создание примеров .service файлов
create_service_files() {
    log_info "Создаю примеры .service файлов..."
    
    # syslog.service
    cat > /etc/xinit/services/syslog.service << 'EOF'
Name         = syslog
Description  = System logging daemon
ExecStart    = /usr/sbin/syslogd -s
Restart      = 1
RestartDelay = 3
TimeoutStart = 10
Enabled      = 1
EOF
    
    # network.service
    cat > /etc/xinit/services/network.service << 'EOF'
Name         = network
Description  = Network initialization
ExecStart    = /bin/sh -c 'sleep 1'
OneShot      = 1
Restart      = 0
TimeoutStart = 10
Enabled      = 1
EOF
    
    # sshd.service
    cat > /etc/xinit/services/sshd.service << 'EOF'
Name         = sshd
Description  = OpenSSH daemon
After        = network, syslog
ExecStart    = /usr/sbin/sshd -D
Restart      = 1
RestartDelay = 5
TimeoutStart = 10
Enabled      = 1
EOF
    
    # cron.service
    cat > /etc/xinit/services/cron.service << 'EOF'
Name         = cron
Description  = Cron daemon
After        = syslog
ExecStart    = /usr/sbin/cron -s
Restart      = 1
RestartDelay = 5
TimeoutStart = 10
Enabled      = 1
EOF
    
    chmod 644 /etc/xinit/services/*.service
    log_ok "Service файлы созданы"
}

# Добавление в rc.conf
add_rc_conf() {
    log_info "Добавляю xinit в /etc/rc.conf..."
    
    # Проверяем есть ли уже
    if grep -q "xinit_enable" /etc/rc.conf 2>/dev/null; then
        log_warn "xinit_enable уже в /etc/rc.conf"
    else
        echo '' >> /etc/rc.conf
        echo '# Xinit service manager' >> /etc/rc.conf
        echo 'xinit_enable="YES"' >> /etc/rc.conf
        log_ok "Добавлено в /etc/rc.conf"
    fi
}

# Вывод информации об установке
show_info() {
    log_ok "Xinit успешно установлен!"
    
    echo ""
    echo -e "${BLUE}=== Информация об установке ===${NC}"
    echo "Binaries:"
    echo "  /sbin/xinit  - PID 1 init"
    echo "  /sbin/xinictl - service manager"
    echo ""
    echo "Конфигурация:"
    echo "  /etc/xinit/services/ - .service файлы"
    echo "  /etc/rc.d/xinit      - rc.d скрипт"
    echo ""
    echo "Логирование:"
    echo "  /var/log/xinit/      - логи сервисов"
    echo "  /var/log/xinit.log   - основной лог"
    echo ""
    echo -e "${BLUE}=== Команды ===${NC}"
    echo "  service xinit start     - запустить xinit"
    echo "  service xinit stop      - остановить xinit"
    echo "  service xinit restart   - перезапустить"
    echo "  xinictl list            - список сервисов"
    echo "  xinictl start <svc>     - запустить сервис"
    echo "  xinictl stop <svc>      - остановить сервис"
    echo ""
    echo -e "${YELLOW}Примечание:${NC} Xinit добавлен в /etc/rc.conf с флагом YES"
    echo "Чтобы отключить при загрузке: sysrc xinit_enable=NO"
    echo ""
}

# Главная функция
main() {
    echo -e "${BLUE}"
    echo "╔════════════════════════════════════════╗"
    echo "║  Xinit Installer for FreeBSD          ║"
    echo "║  No half measures - Just install it!  ║"
    echo "╚════════════════════════════════════════╝"
    echo -e "${NC}"
    echo ""
    
    check_freebsd
    check_root
    
    compile_binaries
    install_binaries
    create_directories
    create_rc_script
    create_service_files
    add_rc_conf
    
    echo ""
    show_info
    
    log_ok "Готово!"
}

# Запуск
main
