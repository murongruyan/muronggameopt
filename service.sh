#!/system/bin/sh
TeamS=${0%/*}

wait_sys_boot_completed() {
	local i=9
	until [ "$(getprop sys.boot_completed)" == "1" ] || [ $i -le 0 ]; do
		i=$((i-1))
		sleep 9
	done
}
wait_sys_boot_completed
sh "$TeamS/vtools/init_vtools.sh" "$(realpath $TeamS/module.prop)"

if [ -f "$TeamS/fengchi_cleanup.sh" ]; then
    /system/bin/sh "$TeamS/fengchi_cleanup.sh" --once
    /system/bin/sh "$TeamS/fengchi_cleanup.sh" >/dev/null 2>&1 &
fi

echo "1" > /proc/sys/walt/sched_conservative_pl
echo "1000" > /proc/oplus-votable/GAUGE_UPDATE/force_val
echo "1" > /proc/oplus-votable/GAUGE_UPDATE/force_active
echo "0" > /sys/module/cpufreq_bouncing/parameters/enable

/system/bin/sh "$TeamS/activity_diaodu.rc" &
