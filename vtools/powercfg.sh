#system/bin/sh
mode=/data/adb/modules/muronggameopt/config/mode.txt

case "$1" in
"powersave" | "standby") echo powersave >$mode ;;
"balance") echo balance >$mode ;;
"performance") echo performance >$mode ;;
"init" | "fast" | "pedestal") echo fast >$mode ;;
esac