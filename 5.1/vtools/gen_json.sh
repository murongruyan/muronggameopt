propPath=$1
version=$(cat $propPath | grep "version=" | cut -d "=" -f2)

json=$(
	cat <<EOF
{
    "name": "慕容调度",
    "author": "慕容茹艳（酷安慕容雪绒）",
    "version": "$version",
    "versionCode": 2025081700,
    "features": {
        "strict": true,
        "pedestal": true
    },
    "module": "muronggameopt",
    "state": "/data/adb/modules/muronggameopt/config/mode.txt",
    "entry": "/data/powercfg.sh"
}
EOF
)
