#!/system/bin/sh

MODDIR=${0%/*}
MODULE_PROP="$MODDIR/module.prop"
SQLITE3_BIN="$MODDIR/bin/sqlite3"
DB_PRIMARY="/data/data/com.oplus.cosa/databases/db_game_database"
DB_SECONDARY="/data/user_de/0/com.oplus.cosa/databases/db_game_database"
LOCK_DIR="/dev/.murong_fengchi_cleanup.lock"
LOG_FILE="$MODDIR/config/fengchi_cleanup.log"
CHECK_INTERVAL=3

read_variant() {
    sed -n 's/^variant=//p' "$MODULE_PROP" 2>/dev/null | head -n 1 | tr -d '\r'
}

log_event() {
    printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >> "$LOG_FILE" 2>/dev/null
}

clear_database() {
    database_path=$1
    [ -f "$database_path" ] || return 2

    "$SQLITE3_BIN" "$database_path" >/dev/null 2>&1 <<'SQL'
.timeout 2000
PRAGMA busy_timeout=2000;
BEGIN IMMEDIATE;
DELETE FROM PackageConfigBean;
COMMIT;
SQL
}

cleanup_cycle() {
    found=0
    failed=0

    for database_path in "$DB_PRIMARY" "$DB_SECONDARY"; do
        [ -f "$database_path" ] || continue
        found=1
        clear_database "$database_path" || failed=1
    done

    [ "$found" -eq 1 ] || return 2
    [ "$failed" -eq 0 ]
}

[ "$(read_variant)" = "third_party" ] || exit 0
[ -x "$SQLITE3_BIN" ] || {
    log_event "未找到可执行的 sqlite3，风驰数据清理未启动"
    exit 0
}

if [ "$1" = "--once" ]; then
    cleanup_cycle
    exit $?
fi

mkdir "$LOCK_DIR" 2>/dev/null || exit 0
trap 'rmdir "$LOCK_DIR" 2>/dev/null' 0
trap 'exit 0' HUP INT TERM

last_status=""
while [ "$(read_variant)" = "third_party" ]; do
    cleanup_cycle
    status=$?
    if [ "$status" != "$last_status" ]; then
        case "$status" in
            0) log_event "已清空风驰 PackageConfigBean，进入循环守护" ;;
            1) log_event "风驰数据库暂时被占用，稍后重试" ;;
            2) log_event "尚未发现风驰数据库，稍后重试" ;;
        esac
        last_status=$status
    fi
    sleep "$CHECK_INTERVAL"
done

log_event "检测到模块版本切换，风驰数据清理已停止"
exit 0
