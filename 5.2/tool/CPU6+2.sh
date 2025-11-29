#!/system/bin/sh
SCRIPT_DIR=$(dirname "$(realpath "$0")")
DIR=$(dirname "$SCRIPT_DIR")
# 遍历目录
MODULE_DIRS=("murongxiancheng" "muronggameopt")
CORE_FILE=""
found=0
for dir in "${MODULE_DIRS[@]}"; do
    module_path="/data/adb/modules/$dir"
    if [ -f "$module_path/applist.conf" ]; then
        FILE_PATH="$module_path/applist.conf"
        found=1
        break
    fi
done

BACKUP_DIR="$DIR"

mkdir -p "$BACKUP_DIR"

BACKUP_FILE="$BACKUP_DIR/applist.conf_$(date +%Y%m%d_%H%M%S).bak"

if [ -f "$FILE_PATH" ]; then
    cp "$FILE_PATH" "$BACKUP_FILE"
    echo "已备份原文件到 $BACKUP_FILE"
fi
	echo "#将QQ音乐主进程绑定0-3
com.tencent.qqmusic=0-3 : NICE 19

#将微信输入法进程绑定0-3
com.tencent.wetype:play=0-3 : NICE 19

#将酷狗音乐后台播放的子进程绑定0-3
com.kugou.android.support=0-3 : NICE 19
com.kugou.android.message=0-3 : NICE 19

#将微信Push消息推送进程绑定0-3
com.tencent.mm:push=0-3 : NICE 19

#系统桌面
com.android.launcher=0-3 : NICE 19
com.android.launcher{*Thread*}=0-5 : SCHED_RR 40
com.android.launcher{binder*}=0-5 : NICE -10
com.android.launcher{ndroid.launcher}=0-5 : NICE -10

# mt管理器
bin.mt.plus=0-3 : NICE 19

# mt管理器共存版
bin.mt.plus.canary=0-3 : NICE 19

# 番茄小说
com.dragon.read=0-3 : NICE 19

# 酷安
com.coolapk.market=0-3 : NICE 19
com.coolapk.market{.coolapk.market}=0-5 : NICE 0
com.coolapk.market{*Thread*}=0-5 : SCHED_RR 40
com.coolapk.market{binder:*}=0-5 : NICE 0

# 小红书
com.xingin.xhs=0-3 : NICE 19
com.xingin.xhs{HeapTaskDaemon}=0-5 : NICE 0
com.xingin.xhs{videodec_*}=0-5 : NICE 0

# 微博轻享版
com.weico.international=0-3 : NICE 19
com.weico.international{*Thread*}=0-5 : SCHED_RR 40
com.weico.international{o.international}=0-5 : NICE 0
com.weico.international{glide-source-th}=0-5 : NICE 0
com.weico.international{binder:*}=0-5 : NICE 0

# 微博
com.sina.weibo=0-3 : NICE 19
com.sina.weibo{com.sina.weibo}=0-5 : NICE 0
com.sina.weibo{*Thread*}=0-5 : SCHED_RR 40
com.sina.weibo{Thread-*}=0-5 : NICE 0

# 闲鱼
com.taobao.idlefish=0-3 : NICE 19
com.taobao.idlefish{*Thread*}=0-5 : SCHED_RR 40
com.taobao.idlefish{1.ui}=0-5 : NICE 0
com.taobao.idlefish{taobao.idlefish}=0-5 : NICE 0
com.taobao.idlefish{1.raster}=0-5 : NICE 0

# 淘宝
com.taobao.taobao=0-3 : NICE 19
com.taobao.taobao{WeexJSBridgeTh}=0-5 : NICE 0
com.taobao.taobao{HeapTaskDaemon}=0-5 : NICE 0
com.taobao.taobao{m.taobao.taobao}=0-5 : NICE 0
com.taobao.taobao{8RYPVI8EZKhJUU}=0-5 : NICE 0

# 京东
com.jingdong.app.mall=0-3 : NICE 19
com.jingdong.app.mall{RunnerWrapper_8}=0-5 : NICE 0
com.jingdong.app.mall{ngdong.app.mall}=0-5 : NICE 0
com.jingdong.app.mall{pool-15-thread-}=0-5 : NICE 0
com.jingdong.app.mall{JDFileDownloade}=0-5 : NICE 0
com.jingdong.app.mall{*Thread*}=0-5 : SCHED_RR 40

# 拼多多
com.xunmeng.pinduoduo=0-3 : NICE 19
com.xunmeng.pinduoduo{Chat#Single-Syn}=0-5 : NICE 0
com.xunmeng.pinduoduo{Startup#RTDispa}=0-5 : NICE 0
com.xunmeng.pinduoduo{nmeng.pinduoduo}=0-5 : NICE 0
com.xunmeng.pinduoduo{*Thread*}=0-5 : SCHED_RR 40
com.xunmeng.pinduoduo{Jit thread pool}=0-5 : NICE 0

# 今日头条
com.ss.android.article.news=0-3 : NICE 19
com.ss.android.article.news{platform-single}=0-5 : NICE 0
com.ss.android.article.news{id.article.news}=0-5 : NICE 0
com.ss.android.article.news{*Thread*}=0-5 : SCHED_RR 40
com.ss.android.article.news{MediaCodec_*}=0-5 : NICE 0

# 知乎
com.zhihu.android=0-3 : NICE 19
com.zhihu.android{m.zhihu.android}=0-5 : NICE 0
com.zhihu.android{*Thread*}=0-5 : SCHED_RR 40

# 钉钉
com.alibaba.android.rimet=0-3 : NICE 19
com.alibaba.android.rimet={a.android.rimet}=0-5 : NICE 0
com.alibaba.android.rimet={*Thread*}=0-5 : SCHED_RR 40
com.alibaba.android.rimet={Doraemon-Proces}=0-5 : NICE 0

# 高德地图
com.autonavi.minimap=0-3 : NICE 19
com.autonavi.minimap{AJXBizCheck}=0-5 : NICE 0
com.autonavi.minimap{JavaScriptThrea}=0-5 : NICE 0
com.autonavi.minimap{Map-Logical-0}=0-5 : NICE 0
com.autonavi.minimap{utonavi.minimap}=0-5 : NICE 0

# 百度地图
com.baidu.BaiduMap=0-3 : NICE 19
com.baidu.BaiduMap{31.1_0223536945}=0-5 : NICE 0
com.baidu.BaiduMap{.31.1_062565145}=0-5 : NICE 0
com.baidu.BaiduMap{.baidu.BaiduMap}=0-5 : NICE 0
com.baidu.BaiduMap{*Thread*}=0-5 : SCHED_RR 40

# 哔哩哔哩
tv.danmaku.bili=0-3 : NICE 19
tv.danmaku.bili{tv.danmaku.bili}=0-5 : SCHED_RR 30
tv.danmaku.bili{*Thread*}=0-5 : SCHED_RR 50
tv.danmaku.bili{IJK_External_Re}=0-5 : SCHED_RR 30

# 抖音
com.ss.android.ugc.aweme=0-3 : NICE 19
com.ss.android.ugc.aweme{*rThread*}=0-5 : SCHED_RR 40
com.ss.android.ugc.aweme{*VDecod*}=0-5 : SCHED_RR 50
com.ss.android.ugc.aweme{*AudioTrack*}=0-5 : SCHED_RR 45
com.ss.android.ugc.aweme{HeapTaskDaemon}=0-5 : NICE 0
com.ss.android.ugc.aweme{*Codec*}=0-4 : NICE 0
com.ss.android.ugc.aweme{#pty-*}=0-4 : NICE 0

# 快手
com.smile.gifmaker=0-3 : NICE 19
com.smile.gifmaker{*Thread*}=0-5 : SCHED_RR 40
com.smile.gifmaker{MediaCodec_*}=0-5 : NICE 0

# 微信
com.tencent.mm=0-3 : NICE 19
com.tencent.mm{com.tencent.mm}=0-5 : NICE 0
com.tencent.mm{default_matrix_}=0-5 : NICE 0
com.tencent.mm{binder:*}=0-5 : NICE 0

# 支付宝
com.eg.android.AlipayGphone=0-3 : NICE 19
com.eg.android.AlipayGphone{crv-worker-thre}=0-5 : NICE 0
com.eg.android.AlipayGphone{id.AlipayGphone}=0-5 : NICE 0
com.eg.android.AlipayGphone{*Thread*}=0-5 : SCHED_RR 40

# QQ
com.tencent.mobileqq=0-3 : NICE 19
com.tencent.mobileqq{encent.mobileqq}=0-5 : NICE 0
com.tencent.mobileqq{*Thread*}=0-5 : SCHED_RR 40
com.tencent.mobileqq{MediaCodec_loop}=0-5 : NICE 0

# 王者荣耀
com.tencent.tmgp.sgame{UnityMain*}=6-7 : SCHED_RR 90
com.tencent.tmgp.sgame{UnityGfx*}=6-7 : SCHED_RR 80
com.tencent.tmgp.sgame{Job.worker*}=0-5 : SCHED_RR 40
com.tencent.tmgp.sgame{Thread-*}=0-5 : NICE 10
com.tencent.tmgp.sgame=0-5 : NICE 19

#王者荣耀国际版
com.levelinfinite.sgameGlobal.midaspay{UnityMain*}=6-7 : SCHED_RR 90
com.levelinfinite.sgameGlobal.midaspay{UnityGfx*}=6-7 : SCHED_RR 80
com.levelinfinite.sgameGlobal.midaspay{Job.worker*}=0-5 : SCHED_RR 40
com.levelinfinite.sgameGlobal.midaspay{Thread-*}=0-5 : NICE 10
com.levelinfinite.sgameGlobal.midaspay=0-5 : NICE 19

#王者荣耀国际版
com.levelinfinite.sgameGlobal{UnityMain*}=6-7 : SCHED_RR 90
com.levelinfinite.sgameGlobal{UnityGfx*}=6-7 : SCHED_RR 80
com.levelinfinite.sgameGlobal{Job.worker*}=0-5 : SCHED_RR 40
com.levelinfinite.sgameGlobal{Thread-*}=0-5 : NICE 10
com.levelinfinite.sgameGlobal=0-5 : NICE 19

#球球大作战
com.ztgame.bob{UnityMain*}=6-7 : SCHED_RR 90
com.ztgame.bob{UnityGxDevie}=6-7 : SCHED_RR 80
com.ztgame.bob{Job.worker*}=0-5 : SCHED_RR 40
com.ztgame.bob{Thread-*}=0-5 : NICE 10
com.ztgame.bob=0-5 : NICE 19

#跑跑卡丁车
com.tencent.tmgp.WePop{UnityMain*}=6-7 : SCHED_RR 90
com.tencent.tmgp.WePop{Job.worker*}=0-5 : SCHED_RR 40
com.tencent.tmgp.WePop{UnityChoreograp}=6-7 : SCHED_RR 80
com.tencent.tmgp.WePop{Thread-*}=0-5 : NICE 10
com.tencent.tmgp.WePop=0-5 : NICE 19

#元气骑士
com.ChillyRoom.DungeonShooter{Thread 0x0xb400}=6-7 : SCHED_RR 90
com.ChillyRoom.DungeonShooter{UnityMain*}=6-7 : SCHED_RR 90
com.ChillyRoom.DungeonShooter{UnityGfx*}=6-7 : SCHED_RR 80
com.ChillyRoom.DungeonShooter{Job.worker*}=0-5 : SCHED_RR 40
com.ChillyRoom.DungeonShooter{Thread-*}=0-5 : NICE 10
com.ChillyRoom.DungeonShooter=0-5 : NICE 19

#少女前线
com.Sunborn.SnqxExilium{UnityMain*}=6-7 : SCHED_RR 90
com.Sunborn.SnqxExilium{UnityGfx*}=6-7 : SCHED_RR 80
com.Sunborn.SnqxExilium{Thread*}=0-5 : NICE 19 : UCLAMP 30 70
com.Sunborn.SnqxExilium{Thread-*}=0-5 : NICE 10
com.Sunborn.SnqxExilium=0-5 : NICE 19

#第五人格
com.netease.dwrg.nearme.gamecenter{Thread-*}=6-7 : SCHED_RR 90
com.netease.dwrg.nearme.gamecenter{NativeThread}=6-7 : SCHED_RR 80
com.netease.dwrg.nearme.gamecenter{Thread-*}=0-5 : NICE 10
com.netease.dwrg.nearme.gamecenter=0-5 : NICE 19

# 原神
com.miHoYo.Yuanshen{UnityMain*}=6-7 : SCHED_RR 90
com.miHoYo.Yuanshen{UnityGfx*}=6-7 : SCHED_RR 80
com.miHoYo.Yuanshen{Job.worker*}=0-5 : SCHED_RR 40
com.miHoYo.Yuanshen{Thread-*}=0-5 : NICE 10
com.miHoYo.Yuanshen=0-5 : NICE 19

# 星穹铁道
com.miHoYo.hkrpg{UnityMain*}=6-7 : SCHED_RR 90
com.miHoYo.hkrpg{UnityGfx*}=6-7 : SCHED_RR 80
com.miHoYo.hkrpg{Job.worker*}=0-5 : SCHED_RR 40
com.miHoYo.hkrpg{Thread-*}=0-5 : NICE 10
com.miHoYo.hkrpg=0-5 : NICE 19

# 和平精英
com.tencent.tmgp.pubgmhd{Thread-[0-9]}=6-7 : SCHED_RR 90
com.tencent.tmgp.pubgmhd{Thread-[1-2][0-9]}=6-7 : SCHED_RR 90
com.tencent.tmgp.pubgmhd{*rThread*}=6-7 : SCHED_RR 80
com.tencent.tmgp.pubgmhd{Audio*}=0-5
com.tencent.tmgp.pubgmhd==0-5 : NICE 19

# 英雄联盟手游
com.tencent.lolm{UnityMain*}=6-7 : SCHED_RR 90
com.tencent.lolm{LogicThread*}=6-7 : SCHED_RR 80
com.tencent.lolm{Job.worker*}=0-5 : SCHED_RR 40
com.tencent.lolm{Thread-*}=0-5 : NICE 10
com.tencent.lolm=0-5 : NICE 19

# QQ飞车
com.tencent.tmgp.speedmobile{UnityMain*}=6-7 : SCHED_RR 90
com.tencent.tmgp.speedmobile{UnityGfx*}=6-7 : SCHED_RR 80
com.tencent.tmgp.speedmobile{Job.worker*}=0-5 : SCHED_RR 40
com.tencent.tmgp.speedmobile{Thread-*}=0-5 : NICE 10
com.tencent.tmgp.speedmobile=0-5 : NICE 19

# 穿越火线：枪战王者
com.tencent.tmgp.cf{UnityMain*}=6-7 : SCHED_RR 90
com.tencent.tmgp.cf{UnityGfx*}=6-7 : SCHED_RR 80
com.tencent.tmgp.cf{Job.worker*}=0-5 : SCHED_RR 40
com.tencent.tmgp.cf{Thread-*}=0-5 : NICE 10
com.tencent.tmgp.cf=0-5 : NICE 19

# 永劫无间手游
com.netease.l22{UnityMain*}=6-7 : SCHED_RR 90
com.netease.l22{UnityGfx*}=6-7 : SCHED_RR 80
com.netease.l22{Job.worker*}=0-5 : SCHED_RR 40
com.netease.l22{Thread-*}=0-5 : NICE 10
com.netease.l22=0-5 : NICE 19

# 守塔不能停
com.jwestern.m4d{UnityMain*}=6-7 : SCHED_RR 90
com.jwestern.m4d{UnityGfx*}=6-7 : SCHED_RR 80
com.jwestern.m4d{Job.worker*}=0-5 : SCHED_RR 40
com.jwestern.m4d{Thread-*}=0-5 : NICE 10
com.jwestern.m4d=0-5 : NICE 19

# 炉石传说国际服
com.hearthstone{UnityMain*}=6-7 : SCHED_RR 90
com.hearthstone{UnityGfx*}=6-7 : SCHED_RR 80
com.hearthstone{Job.worker*}=0-5 : SCHED_RR 40
com.hearthstone{Thread-*}=0-5 : NICE 10
com.hearthstone=0-5 : NICE 19

# 阴阳师
com.netease.onmyoji{UnityMain*}=6-7 : SCHED_RR 90
com.netease.onmyoji{UnityGfx*}=6-7 : SCHED_RR 80
com.netease.onmyoji{Job.worker*}=0-5 : SCHED_RR 40
com.netease.onmyoji{Thread-*}=0-5 : NICE 10
com.netease.onmyoji=0-5 : NICE 19

# 崩坏3
com.miHoYo.bh3.uc{UnityMain*}=6-7 : SCHED_RR 90
com.miHoYo.bh3.uc{UnityGfx*}=6-7 : SCHED_RR 80
com.miHoYo.bh3.uc{Job.worker*}=0-5 : SCHED_RR 40
com.miHoYo.bh3.uc{Thread-*}=0-5 : NICE 10
com.miHoYo.bh3.uc=0-5 : NICE 19

# 崩坏3
com.miHoYo.bh3.mi{UnityMain*}=6-7 : SCHED_RR 90
com.miHoYo.bh3.mi{UnityGfx*}=6-7 : SCHED_RR 80
com.miHoYo.bh3.mi{Job.worker*}=0-5 : SCHED_RR 40
com.miHoYo.bh3.mi{Thread-*}=0-5 : NICE 10
com.miHoYo.bh3.mi=0-5 : NICE 19

# 崩坏3
com.miHoYo.enterprise.NGHSoD{UnityMain*}=6-7 : SCHED_RR 90
com.miHoYo.enterprise.NGHSoD{UnityGfx*}=6-7 : SCHED_RR 80
com.miHoYo.enterprise.NGHSoD{Job.worker*}=0-5 : SCHED_RR 40
com.miHoYo.enterprise.NGHSoD{Thread-*}=0-5 : NICE 10
com.miHoYo.enterprise.NGHSoD=0-5 : NICE 19

# 崩坏3
com.miHoYo.bh3.bilibili{UnityMain*}=6-7 : SCHED_RR 90
com.miHoYo.bh3.bilibili{UnityGfx*}=6-7 : SCHED_RR 80
com.miHoYo.bh3.bilibili{Job.worker*}=0-5 : SCHED_RR 40
com.miHoYo.bh3.bilibili{Thread-*}=0-5 : NICE 10
com.miHoYo.bh3.bilibili=0-5 : NICE 19

# 明日方舟
com.hypergryph.arknights{UnityMain*}=6-7 : SCHED_RR 90
com.hypergryph.arknights{UnityGfx*}=6-7 : SCHED_RR 80
com.hypergryph.arknights{Job.worker*}=0-5 : SCHED_RR 40
com.hypergryph.arknights{Thread-*}=0-5 : NICE 10
com.hypergryph.arknights=0-5 : NICE 19

# 星球重启
com.hermes.j1game.mi{UnityMain*}=6-7 : SCHED_RR 90
com.hermes.j1game.mi{UnityGfx*}=6-7 : SCHED_RR 80
com.hermes.j1game.mi{Job.worker*}=0-5 : SCHED_RR 40
com.hermes.j1game.mi{Thread-*}=0-5 : NICE 10
com.hermes.j1game.mi=0-5 : NICE 19

# 星球重启
com.hermes.j1game{UnityMain*}=6-7 : SCHED_RR 90
com.hermes.j1game{UnityGfx*}=6-7 : SCHED_RR 80
com.hermes.j1game{Job.worker*}=0-5 : SCHED_RR 40
com.hermes.j1game{Thread-*}=0-5 : NICE 10
com.hermes.j1game=0-5 : NICE 19

# 星球重启
com.hermes.j1game.aligames{UnityMain*}=6-7 : SCHED_RR 90
com.hermes.j1game.aligames{UnityGfx*}=6-7 : SCHED_RR 80
com.hermes.j1game.aligames{Job.worker*}=0-5 : SCHED_RR 40
com.hermes.j1game.aligames{Thread-*}=0-5 : NICE 10
com.hermes.j1game.aligames=0-5 : NICE 19

# 星球重启
com.hermes.j1game.huawei{UnityMain*}=6-7 : SCHED_RR 90
com.hermes.j1game.huawei{UnityGfx*}=6-7 : SCHED_RR 80
com.hermes.j1game.huawei{Job.worker*}=0-5 : SCHED_RR 40
com.hermes.j1game.huawei{Thread-*}=0-5 : NICE 10
com.hermes.j1game.huawei=0-5 : NICE 19

# 星球重启
com.hermes.j1game.vivo{UnityMain*}=6-7 : SCHED_RR 90
com.hermes.j1game.vivo{UnityGfx*}=6-7 : SCHED_RR 80
com.hermes.j1game.vivo{Job.worker*}=0-5 : SCHED_RR 40
com.hermes.j1game.vivo{Thread-*}=0-5 : NICE 10
com.hermes.j1game.vivo=0-5 : NICE 19

# 地下城与勇士手游
com.tencent.tmgp.dnf{UnityMain*}=6-7 : SCHED_RR 90
com.tencent.tmgp.dnf{UnityGfx*}=6-7 : SCHED_RR 80
com.tencent.tmgp.dnf{Job.worker*}=0-5 : SCHED_RR 40
com.tencent.tmgp.dnf{Thread-*}=0-5 : NICE 10
com.tencent.tmgp.dnf=0-5 : NICE 19

# 逆水寒手游
com.netease.nshm{UnityMain*}=6-7 : SCHED_RR 90
com.netease.nshm{UnityGfx*}=6-7 : SCHED_RR 80
com.netease.nshm{Job.worker*}=0-5 : SCHED_RR 40
com.netease.nshm{Thread-*}=0-5 : NICE 10
com.netease.nshm=0-5 : NICE 19

# 学园偶像大师
com.bandainamcoent.idolmaster_gakuen{UnityMain*}=6-7 : SCHED_RR 90
com.bandainamcoent.idolmaster_gakuen{UnityGfx*}=6-7 : SCHED_RR 80
com.bandainamcoent.idolmaster_gakuen{Job.worker*}=0-5 : SCHED_RR 40
com.bbandainamcoent.idolmaster_gakuen{Thread-*}=0-5 : NICE 10
com.bandainamcoent.idolmaster_gakuen=0-5 : NICE 19

# 猎魂觉醒网易
com.netease.AVALON{UnityMain*}=6-7 : SCHED_RR 90
com.netease.AVALON{UnityGfx*}=6-7 : SCHED_RR 80
com.netease.AVALON{Job.worker*}=0-5 : SCHED_RR 40
com.netease.AVALON{Thread-*}=0-5 : NICE 10
com.netease.AVALON=0-5 : NICE 19

# 深空之眼
com.yongshi.tenojo.ys{UnityMain*}=6-7 : SCHED_RR 90
com.yongshi.tenojo.ys{UnityGfx*}=6-7 : SCHED_RR 80
com.yongshi.tenojo.ys{Job.worker*}=0-5 : SCHED_RR 40
com.yongshi.tenojo.ys{Thread-*}=0-5 : NICE 10
com.yongshi.tenojo.ys=0-5 : NICE 19

# 部落冲突
com.tencent.tmgp.supercell.clashofclans{UnityMain*}=6-7 : SCHED_RR 90
com.tencent.tmgp.supercell.clashofclans{UnityGfx*}=6-7 : SCHED_RR 80
com.tencent.tmgp.supercell.clashofclans{Job.worker*}=0-5 : SCHED_RR 40
com.tencent.tmgp.supercell.clashofclans{Thread-*}=0-5 : NICE 10
com.tencent.tmgp.supercell.clashofclans=0-5 : NICE 19

# 海岛奇兵国际服
com.supercell.boombeach{UnityMain*}=6-7 : SCHED_RR 90
com.supercell.boombeach{UnityGfx*}=6-7 : SCHED_RR 80
com.supercell.boombeach{Job.worker*}=0-5 : SCHED_RR 40
com.supercell.boombeach{Thread-*}=0-5 : NICE 10
com.supercell.boombeach=0-5 : NICE 19

# 幻塔
com.pwrd.hotta.laohu{UnityMain*}=6-7 : SCHED_RR 90
com.pwrd.hotta.laohu{UnityGfx*}=6-7 : SCHED_RR 80
com.pwrd.hotta.laohu{Job.worker*}=0-5 : SCHED_RR 40
com.pwrd.hotta.laohu{Thread-*}=0-5 : NICE 10
com.pwrd.hotta.laohu=0-5 : NICE 19

# 火影忍者
com.tencent.KiHan{UnityMain*}=6-7 : SCHED_RR 90
com.tencent.KiHan{UnityGfx*}=6-7 : SCHED_RR 80
com.tencent.KiHan{Job.worker*}=0-5 : SCHED_RR 40
com.tencent.KiHan{Thread-*}=0-5 : NICE 10
com.tencent.KiHan=0-5 : NICE 19

# 金铲铲之战
com.tencent.jkchess{UnityMain*}=6-7 : SCHED_RR 90
com.tencent.jkchess{UnityGfx*}=6-7 : SCHED_RR 80
com.tencent.jkchess{Job.worker*}=0-5 : SCHED_RR 40
com.tencent.jkchess{Thread-*}=0-5 : NICE 10
com.tencent.jkchess=0-5 : NICE 19

# 使命召唤手游
com.tencent.tmgp.cod{UnityMain*}=6-7 : SCHED_RR 90
com.tencent.tmgp.cod{UnityGfx*}=6-7 : SCHED_RR 80
com.tencent.tmgp.cod{Job.worker*}=0-5 : SCHED_RR 40
com.tencent.tmgp.cod{Thread-*}=0-5 : NICE 10
com.tencent.tmgp.cod=0-5 : NICE 19

# 碧蓝航线
com.bilibili.azurlane{UnityMain*}=6-7 : SCHED_RR 90
com.bilibili.azurlane{UnityGfx*}=6-7 : SCHED_RR 80
com.bilibili.azurlane{Job.worker*}=0-5 : SCHED_RR 40
com.bilibili.azurlane{Thread-*}=0-5 : NICE 10
com.bilibili.azurlane=0-5 : NICE 19

# 战双帕弥什
com.kurogame.haru.aligames{UnityMain*}=6-7 : SCHED_RR 90
com.kurogame.haru.aligames{UnityGfx*}=6-7 : SCHED_RR 80
com.kurogame.haru.aligames{Job.worker*}=0-5 : SCHED_RR 40
com.kurogame.haru.aligames{Thread-*}=0-5 : NICE 10
com.kurogame.haru.aligames=0-5 : NICE 19

# 战双帕弥什
com.kurogame.haru.hero{UnityMain*}=6-7 : SCHED_RR 90
com.kurogame.haru.hero{UnityGfx*}=6-7 : SCHED_RR 80
com.kurogame.haru.hero{Job.worker*}=0-5 : SCHED_RR 40
com.kurogame.haru.hero{Thread-*}=0-5 : NICE 10
com.kurogame.haru.hero=0-5 : NICE 19

# 来自星尘
com.hypergryph.exastris{UnityMain*}=6-7 : SCHED_RR 90
com.hypergryph.exastris{UnityGfx*}=6-7 : SCHED_RR 80
com.hypergryph.exastris{Job.worker*}=0-5 : SCHED_RR 40
com.hypergryph.exastris{Thread-*}=0-5 : NICE 10
com.hypergryph.exastris=0-5 : NICE 19

# 重返未来1999
com.shenlan.m.reverse1999{UnityMain*}=6-7 : SCHED_RR 90
com.shenlan.m.reverse1999{UnityGfx*}=6-7 : SCHED_RR 80
com.shenlan.m.reverse1999{Job.worker*}=0-5 : SCHED_RR 40
com.shenlan.m.reverse1999{Thread-*}=0-5 : NICE 10
com.shenlan.m.reverse1999=0-5 : NICE 19

# 蛋仔派对
com.netease.party.nearme.gamecenter{UnityMain*}=6-7 : SCHED_RR 90
com.netease.party.nearme.gamecenter{UnityGfx*}=6-7 : SCHED_RR 80
com.netease.party.nearme.gamecenter{Job.worker*}=0-5 : SCHED_RR 40
com.netease.party.nearme.gamecenter{Thread-*}=0-5 : NICE 10
com.netease.party.nearme.gamecenter=0-5 : NICE 19

# 蛋仔派对
com.netease.party{UnityMain*}=6-7 : SCHED_RR 90
com.netease.party{UnityGfx*}=6-7 : SCHED_RR 80
com.netease.party{Job.worker*}=0-5 : SCHED_RR 40
com.netease.party{Thread-*}=0-5 : NICE 10
com.netease.party=0-5 : NICE 19

# 元梦之星
com.tencent.letsgo{UnityMain*}=6-7 : SCHED_RR 90
com.tencent.letsgo{UnityGfx*}=6-7 : SCHED_RR 80
com.tencent.letsgo{Job.worker*}=0-5 : SCHED_RR 40
com.tencent.letsgo{Thread-*}=0-5 : NICE 10
com.tencent.letsgo=0-5 : NICE 19

# NBA 2K20
com.t2ksports.nba2k20and{UnityMain*}=6-7 : SCHED_RR 90
com.t2ksports.nba2k20and{UnityGfx*}=6-7 : SCHED_RR 80
com.t2ksports.nba2k20and{Job.worker*}=0-5 : SCHED_RR 40
com.t2ksports.nba2k20and{Thread-*}=0-5 : NICE 10
com.t2ksports.nba2k20and=0-5 : NICE 19

# 王牌竞速
com.netease.aceracer.aligames{UnityMain*}=6-7 : SCHED_RR 90
com.netease.aceracer.aligames{UnityGfx*}=6-7 : SCHED_RR 80
com.netease.aceracer.aligames{Job.worker*}=0-5 : SCHED_RR 40
com.netease.aceracer.aligames{Thread-*}=0-5 : NICE 10
com.netease.aceracer.aligames=0-5 : NICE 19

# 香肠派对
com.sofunny.Sausage{UnityMain*}=6-7 : SCHED_RR 90
com.sofunny.Sausage{UnityGfx*}=6-7 : SCHED_RR 80
com.sofunny.Sausage{Job.worker*}=0-5 : SCHED_RR 40
com.sofunny.Sausage{Thread-*}=0-5 : NICE 10
com.sofunny.Sausage=0-5 : NICE 19

# 迷你世界
com.playmini.miniworld{UnityMain*}=6-7 : SCHED_RR 90
com.playmini.miniworld{UnityGfx*}=6-7 : SCHED_RR 80
com.playmini.miniworld{Job.worker*}=0-5 : SCHED_RR 40
com.playmini.miniworld{Thread-*}=0-5 : NICE 10
com.playmini.miniworld=0-5 : NICE 19

# 尘白禁区
com.dragonli.projectsnow.lhm{UnityMain*}=6-7 : SCHED_RR 90
com.dragonli.projectsnow.lhm{UnityGfx*}=6-7 : SCHED_RR 80
com.dragonli.projectsnow.lhm{Job.worker*}=0-5 : SCHED_RR 40
com.dragonli.projectsnow.lhm{Thread-*}=0-5 : NICE 10
com.dragonli.projectsnow.lhm=0-5 : NICE 19

# 绝区零
com.miHoYo.Nap{UnityMain*}=6-7 : SCHED_RR 90
com.miHoYo.Nap{UnityGfx*}=6-7 : SCHED_RR 80
com.miHoYo.Nap{Job.worker*}=0-5 : SCHED_RR 40
com.miHoYo.Nap{Thread-*}=0-5 : NICE 10
com.miHoYo.Nap=0-5 : NICE 19

# 使命召唤:战争地带
com.activision.callofduty.warzone{UnityMain*}=6-7 : SCHED_RR 90
com.activision.callofduty.warzone{UnityGfx*}=6-7 : SCHED_RR 80
com.activision.callofduty.warzone{Job.worker*}=0-5 : SCHED_RR 40
com.activision.callofduty.warzone{Thread-*}=0-5 : NICE 10
com.activision.callofduty.warzone{*rThread*}=6-7 : SCHED_RR 80
com.activision.callofduty.warzone{GameThread*}=6-7 : SCHED_RR 90
com.activision.callofduty.warzone=0-5 : NICE 19

# YGOPro3
com.YGO.MDPro3{UnityMain*}=6-7 : SCHED_RR 90
com.YGO.MDPro3{UnityGfx*}=6-7 : SCHED_RR 80
com.YGO.MDPro3{Job.worker*}=0-5 : SCHED_RR 40
com.YGO.MDPro3{Thread-*}=0-5 : NICE 10
com.YGO.MDPro3=0-5 : NICE 19

# 和平精英日韩服
com.pubg.krmobile{Thread-*}=6-7 : SCHED_RR 90
com.pubg.krmobile{*rThread*}=6-7 : SCHED_RR 80
com.pubg.krmobile{Audio*}=0-5
com.pubg.krmobile=0-5 : NICE 19

# 和平精英国际服
com.tencent.ig{GameThread*}=6-7 : SCHED_RR 90
com.tencent.ig{*Thread*}=6-7 : SCHED_RR 80
com.tencent.ig{Audio*}=0-5
com.tencent.ig=0-5 : NICE 19

# 鸣潮
com.kurogame.mingchao{Thread-*}=6-7 : SCHED_RR 90
com.kurogame.mingchao{*rThread*}=6-7 : SCHED_RR 80
com.kurogame.mingchao{GameThread*}=6-7 : SCHED_RR 90
com.kurogame.mingchao{Audio*}=0-5
com.kurogame.mingchao=0-5 : NICE 19

# 晶核
com.hermes.p6game{Thread-*}=6-7 : SCHED_RR 90
com.hermes.p6game{*rThread*}=6-7 : SCHED_RR 80
com.hermes.p6game{GameThread*}=6-7 : SCHED_RR 90
com.hermes.p6game{Audio*}=0-5
com.hermes.p6game=0-5 : NICE 19

# 晶核
com.hermes.p6game.aligames{Thread-*}=6-7 : SCHED_RR 90
com.hermes.p6game.aligames{*rThread*}=6-7 : SCHED_RR 80
com.hermes.p6game.aligames{GameThread*}=6-7 : SCHED_RR 90
com.hermes.p6game.aligames{Audio*}=0-5
com.hermes.p6game.aligames=0-5 : NICE 19

# 晶核
com.hermes.p6game.huawei{Thread-*}=6-7 : SCHED_RR 90
com.hermes.p6game.huawei{*rThread*}=6-7 : SCHED_RR 80
com.hermes.p6game.huawei{GameThread*}=6-7 : SCHED_RR 90
com.hermes.p6game.huawei{Audio*}=0-5
com.hermes.p6game.huawei=0-5 : NICE 19

# 暗区突围
com.tencent.mf.uam{Thread-*}=6-7 : SCHED_RR 90
com.tencent.mf.uam{*rThread*}=6-7 : SCHED_RR 80
com.tencent.mf.uam{GameThread*}=6-7 : SCHED_RR 90
com.tencent.mf.uam{Audio*}=0-5
com.tencent.mf.uam=0-5 : NICE 19

# 巅峰极速
com.netease.race{Thread-*}=6-7 : SCHED_RR 90
com.netease.race{*rThread*}=6-7 : SCHED_RR 80
com.netease.race{GameThread*}=6-7 : SCHED_RR 90
com.netease.race{Audio*}=0-5
com.netease.race=0-5 : NICE 19

# 荒野行动
com.netease.hyxd.nearme.gamecenter{Thread-*}=6-7 : SCHED_RR 90
com.netease.hyxd.nearme.gamecenter{*rThread*}=6-7 : SCHED_RR 80
com.netease.hyxd.nearme.gamecenter{GameThread*}=6-7 : SCHED_RR 90
com.netease.hyxd.nearme.gamecenter{Audio*}=0-5
com.netease.hyxd.nearme.gamecenter=0-5 : NICE 19

# 荒野行动
com.netease.hyxd.aligames{Thread-*}=6-7 : SCHED_RR 90
com.netease.hyxd.aligames{*rThread*}=6-7 : SCHED_RR 80
com.netease.hyxd.aligames{GameThread*}=6-7 : SCHED_RR 90
com.netease.hyxd.aligames=0-5 : NICE 19

# 三角洲行动
com.tencent.tmgp.dfm{*rThread*}=6-7 : SCHED_RR 90
com.tencent.tmgp.dfm{GameThread*}=6-7 : SCHED_RR 80
com.tencent.tmgp.dfm=0-5 : NICE 19

# 极品飞车:集结
com.tencent.nfsonline{UnityMain*}=6-7 : SCHED_RR 90
com.tencent.nfsonline{UnityGfx*}=6-7 : SCHED_RR 80
com.tencent.nfsonline{Job.worker*}=0-5 : SCHED_RR 40
com.tencent.nfsonline{Thread-*}=0-5 : NICE 10
com.tencent.nfsonline{*rThread*}=6-7 : SCHED_RR 80
com.tencent.nfsonline{GameThread*}=6-7 : SCHED_RR 90
com.tencent.nfsonline=0-5 : NICE 19

# 燕云十六声
com.netease.yyslscn{Thread-*}=6-7 : SCHED_RR 90
com.netease.yyslscn{*rThread*}=6-7 : SCHED_RR 80
com.netease.yyslscn{GameThread*}=6-7 : SCHED_RR 90
com.netease.yyslscn=0-5 : NICE 19

# Pro象棋
vip.wqby.pro{*Thread*}=0-5 : NICE 19
vip.wqby.pro{binder*}=0-5 : NICE 19
vip.wqby.pro{Job.worker*}=0-5 : SCHED_RR 40
vip.wqby.pro=0-3 : NICE 19

# JJ象棋
cn.jj.chess{UnityMain*}=6-7 : SCHED_RR 90
cn.jj.chess{UnityGfx*}=6-7 : SCHED_RR 80
cn.jj.chess{Job.worker*}=0-5 : SCHED_RR 40
cn.jj.chess{Thread-*}=0-5 : NICE 10
cn.jj.chess=0-5 : NICE 19

# 棋路
cn.apppk.apps.chessroad{*Thread*}=0-5 : NICE 19
cn.apppk.apps.chessroad{binder*}=0-5 : NICE 19
cn.apppk.apps.chessroad{Thread-*}=0-5 : NICE 19
cn.apppk.apps.chessroad=0-3 : NICE 19

# 一念逍遥
com.leiting.xian{UnityMain*}=6-7 : SCHED_RR 90
com.leiting.xian{UnityGfx*}=6-7 : SCHED_RR 80
com.leiting.xian{Job.worker*}=0-5 : SCHED_RR 40
com.leiting.xian{Thread-*}=0-5 : NICE 10
com.leiting.xian=0-5 : NICE 19

# 狼人杀(网易)
com.netease.lrs{UnityMain*}=6-7 : SCHED_RR 90
com.netease.lrs{UnityGfx*}=6-7 : SCHED_RR 80
com.netease.lrs{Job.worker*}=0-5 : SCHED_RR 40
com.netease.lrs{Thread-*}=0-5 : NICE 10
com.netease.lrs=0-5 : NICE 19

# 无畏契约
com.tencent.tmgp.codev{GameThread}=6-7 : SCHED_RR 90
com.tencent.tmgp.codev{NativeThread*}=6-7 : SCHED_RR 80
com.tencent.tmgp.codev{PoolThread*}=6-7 : SCHED_RR 40
com.tencent.tmgp.codev{Thread-*}=0-5 : NICE 10
com.tencent.tmgp.codev=0-5 : NICE 19">"$DIR/applist.conf"

echo "* 6+2线程配置执行完成"