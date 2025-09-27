#!/system/bin/sh
BASEDIR="$(dirname $(readlink -f "$0"))"

source $BASEDIR/gen_json.sh $1
echo "$json" >/data/powercfg.json

cp -af $BASEDIR/powercfg.sh /data/powercfg.sh
chmod 755 /data/powercfg.sh