#!/system/bin/sh
SCRIPT_DIR=$(dirname "$(realpath "$0")")
DIR=$(dirname "$SCRIPT_DIR")

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

# 备份目录为上层目录
BACKUP_DIR="$DIR"

# 创建备份目录如果不存在
mkdir -p "$BACKUP_DIR"

# 备份文件名，含时间戳
BACKUP_FILE="$BACKUP_DIR/applist.conf_$(date +%Y%m%d_%H%M%S).bak"

# 检查原文件是否存在，如果存在则进行备份
if [ -f "$FILE_PATH" ]; then
    cp "$FILE_PATH" "$BACKUP_FILE"
    echo "已备份原文件到 $BACKUP_FILE"
fi
	echo "#将QQ音乐主进程绑定5-6
com.tencent.qqmusic=5-6

#将微信输入法进程绑定5-6
com.tencent.wetype:play=5-6

#将酷狗音乐后台播放的子进程绑定5-6
com.kugou.android.support=5-6
com.kugou.android.message=5-6

#将微信Push消息推送进程绑定5-6
com.tencent.mm:push=5-6

#系统桌面
com.android.launcher=5-6
com.android.launcher{*Thread*}=2-6
com.android.launcher{binder*}=2-6
com.android.launcher{ndroid.launcher}=2-6

# mt管理器
bin.mt.plus=5-6

# mt管理器共存版
bin.mt.plus.canary=5-6

# 番茄小说
com.dragon.read=5-6

# 酷安
com.coolapk.market=5-6
com.coolapk.market{.coolapk.market}=2-6
com.coolapk.market{*Thread*}=2-6
com.coolapk.market{binder:*}=2-6

# 小红书
com.xingin.xhs=5-6
com.xingin.xhs{HeapTaskDaemon}=2-6
com.xingin.xhs{videodec_*}=2-6

# 微博轻享版
com.weico.international=5-6
com.weico.international{*Thread*}=2-6
com.weico.international{o.international}=2-6
com.weico.international{glide-source-th}=2-6
com.weico.international{binder:*}=2-6

# 微博
com.sina.weibo=5-6
com.sina.weibo{com.sina.weibo}=2-6
com.sina.weibo{*Thread*}=2-6
com.sina.weibo{Thread-*}=2-6

# 闲鱼
com.taobao.idlefish=5-6
com.taobao.idlefish{*Thread*}=2-6
com.taobao.idlefish{1.ui}=2-6
com.taobao.idlefish{taobao.idlefish}=2-6
com.taobao.idlefish{1.raster}=2-6

# 淘宝
com.taobao.taobao=5-6
com.taobao.taobao{WeexJSBridgeTh}=2-6
com.taobao.taobao{HeapTaskDaemon}=2-6
com.taobao.taobao{m.taobao.taobao}=2-6
com.taobao.taobao{8RYPVI8EZKhJUU}=2-6

# 京东
com.jingdong.app.mall=5-6
com.jingdong.app.mall{RunnerWrapper_8}=2-6
com.jingdong.app.mall{ngdong.app.mall}=2-6
com.jingdong.app.mall{pool-15-thread-}=2-6
com.jingdong.app.mall{JDFileDownloade}=2-6
com.jingdong.app.mall{*Thread*}=2-6

# 拼多多
com.xunmeng.pinduoduo=5-6
com.xunmeng.pinduoduo{Chat#Single-Syn}=2-6
com.xunmeng.pinduoduo{Startup#RTDispa}=2-6
com.xunmeng.pinduoduo{nmeng.pinduoduo}=2-6
com.xunmeng.pinduoduo{*Thread*}=2-6
com.xunmeng.pinduoduo{Jit thread pool}=2-6

# 今日头条
com.ss.android.article.news=5-6
com.ss.android.article.news{platform-single}=2-6
com.ss.android.article.news{id.article.news}=2-6
com.ss.android.article.news{*Thread*}=2-6
com.ss.android.article.news{MediaCodec_*}=2-6

# 知乎
com.zhihu.android=5-6
com.zhihu.android{m.zhihu.android}=2-6
com.zhihu.android{*Thread*}=2-6

# 钉钉
com.alibaba.android.rimet=5-6
com.alibaba.android.rimet={a.android.rimet}=2-6
com.alibaba.android.rimet={*Thread*}=2-6
com.alibaba.android.rimet={Doraemon-Proces}=2-6

# 高德地图
com.autonavi.minimap=5-6
com.autonavi.minimap{AJXBizCheck}=2-6
com.autonavi.minimap{JavaScriptThrea}=2-6
com.autonavi.minimap{Map-Logical-0}=2-6
com.autonavi.minimap{utonavi.minimap}=2-6

# 百度地图
com.baidu.BaiduMap=5-6
com.baidu.BaiduMap{31.1_0223536945}=2-6
com.baidu.BaiduMap{.31.1_062565145}=2-6
com.baidu.BaiduMap{.baidu.BaiduMap}=2-6
com.baidu.BaiduMap{*Thread*}=2-6

# 哔哩哔哩
tv.danmaku.bili=5-6
tv.danmaku.bili{tv.danmaku.bili}=2-6
tv.danmaku.bili{*Thread*}=2-6
tv.danmaku.bili{IJK_External_Re}=2-6

# 抖音
com.ss.android.ugc.aweme=5-6
com.ss.android.ugc.aweme{*Thread*}=2-6
com.ss.android.ugc.aweme{VDecod2*}=2-6
com.ss.android.ugc.aweme{droid.ugc.aweme}=2-6
com.ss.android.ugc.aweme{HeapTaskDaemon}=2-6
com.ss.android.ugc.aweme{#pty-wqp-*}=2-6

# 快手
com.smile.gifmaker=5-6
com.smile.gifmaker{*Thread*}=2-6
com.smile.gifmaker{MediaCodec_*}=2-6

# 微信
com.tencent.mm=5-6
com.tencent.mm{com.tencent.mm}=2-6
com.tencent.mm{default_matrix_}=2-6
com.tencent.mm{binder:*}=2-6

# 支付宝
com.eg.android.AlipayGphone=5-6
com.eg.android.AlipayGphone{crv-worker-thre}=2-6
com.eg.android.AlipayGphone{id.AlipayGphone}=2-6
com.eg.android.AlipayGphone{*Thread*}=2-6

# QQ
com.tencent.mobileqq=5-6
com.tencent.mobileqq{encent.mobileqq}=2-6
com.tencent.mobileqq{*Thread*}=2-6
com.tencent.mobileqq{MediaCodec_loop}=2-6

# 王者荣耀
com.tencent.tmgp.sgame{UnityMain*}=7
com.tencent.tmgp.sgame{UnityGfx*}=2-4
com.tencent.tmgp.sgame{Job.worker*}=2-4
com.tencent.tmgp.sgame{Thread*}=2-6
com.tencent.tmgp.sgame{Audio*}=2-4
com.tencent.tmgp.sgame=2-6

#王者荣耀国际版
com.levelinfinite.sgameGlobal.midaspay{UnityMain}=7
com.levelinfinite.sgameGlobal.midaspay{UnityGfxDeviceW}=2-4
com.levelinfinite.sgameGlobal.midaspay{Thread-*}=2-6
com.levelinfinite.sgameGlobal.midaspay{Job.worker*}=2-4
com.levelinfinite.sgameGlobal.midaspay{Audio*}=2-4
com.levelinfinite.sgameGlobal.midaspay=2-6

#王者荣耀国际版
com.levelinfinite.sgameGlobal{UnityMain}=7
com.levelinfinite.sgameGlobal{UnityGfxDeviceW}=2-4
com.levelinfinite.sgameGlobal{Thread-*}=2-6
com.levelinfinite.sgameGlobal{Job.worker*}=2-4
com.levelinfinite.sgameGlobal{Audio*}=2-4
com.levelinfinite.sgameGlobal=2-6

#球球大作战
com.ztgame.bob{UnityMain}=7
com.ztgame.bob{UnityGxDevie}=2-4
com.ztgame.bob{com.ztgame.bob}=2-4
com.ztgame.bob{Thread-*}=2-6
com.ztgame.bob{Audio*}=2-4
com.ztgame.bob=2-6

#跑跑卡丁车
com.tencent.tmgp.WePop{UnityMain}=7
com.tencent.tmgp.WePop{Thread-*}=2-6
com.tencent.tmgp.WePop{UnityChoreograp}=2-4
com.tencent.tmgp.WePop{AsyncReadManage}=2-4
com.tencent.tmgp.WePop{Job.Worker*}=2-4
com.tencent.tmgp.WePop{Audio*}=2-4
com.tencent.tmgp.WePop=2-6

#元气骑士
com.ChillyRoom.DungeonShooter{Thread 0x0xb400}=7
com.ChillyRoom.DungeonShooter{UnityMain}=7
com.ChillyRoom.DungeonShooter{UnityGfx*}=2-4
com.ChillyRoom.DungeonShooter{Job.Worker*}=2-4
com.ChillyRoom.DungeonShooter{Thread-*}=2-6
com.ChillyRoom.DungeonShooter{Audio*}=2-4
com.ChillyRoom.DungeonShooter=2-6

#少女前线
com.Sunborn.SnqxExilium{UnityMain}=7
com.Sunborn.SnqxExilium{UnityGfx*}=2-4
com.Sunborn.SnqxExilium{Job.Worker*}=2-4
com.Sunborn.SnqxExilium{Thread*}=2-4
com.Sunborn.SnqxExilium{Audio*}=2-4
com.Sunborn.SnqxExilium=2-6

#第五人格
com.netease.dwrg.nearme.gamecenter{Thread-*}=7
com.netease.dwrg.nearme.gamecenter{HeapTaskDaemon}=2-4
com.netease.dwrg.nearme.gamecenter{NativeThread}=2-4
com.netease.dwrg.nearme.gamecenter{arme.gamecenter}=2-4
com.netease.dwrg.nearme.gamecenter{Audio*}=2-4
com.netease.dwrg.nearme.gamecenter=2-6

# 原神
com.miHoYo.Yuanshen{UnityMain*}=7
com.miHoYo.Yuanshen{UnityGfx*}=2-4
com.miHoYo.Yuanshen{Thread-*}=2-6
com.miHoYo.Yuanshen{Job.worker*}=2-4
com.miHoYo.Yuanshen{Audio*}=2-4
com.miHoYo.Yuanshen=2-6

# 星穹铁道
com.miHoYo.hkrpg{UnityMain*}=7
com.miHoYo.hkrpg{UnityGfx*}=2-4
com.miHoYo.hkrpg{Thread-*}=2-6
com.miHoYo.hkrpg{Job.worker*}=2-4
com.miHoYo.hkrpg{Audio*}=2-4
com.miHoYo.hkrpg=2-6

# 和平精英
com.tencent.tmgp.pubgmhd{Thread-[0-9]}=7
com.tencent.tmgp.pubgmhd{Thread-[1-2][0-9]}=7
com.tencent.tmgp.pubgmhd{*rThread*}=2-6
com.tencent.tmgp.pubgmhd{Audio*}=2-4
com.tencent.tmgp.pubgmhd=2-6

# 英雄联盟手游
com.tencent.lolm{UnityMain*}=7
com.tencent.lolm{LogicThread*}=2-4
com.tencent.lolm{Thread-*}=2-6
com.tencent.lolm{Job.worker*}=2-4
com.tencent.lolm{Audio*}=2-4
com.tencent.lolm=2-6

# QQ飞车
com.tencent.tmgp.speedmobile{UnityMain*}=7
com.tencent.tmgp.speedmobile{UnityGfx*}=2-4
com.tencent.tmgp.speedmobile{Job.worker*}=2-4
com.tencent.tmgp.speedmobile{Thread-*}=2-6
com.tencent.tmgp.speedmobile{Audio*}=2-4
com.tencent.tmgp.speedmobile=2-6

# 穿越火线：枪战王者
com.tencent.tmgp.cf{UnityMain*}=7
com.tencent.tmgp.cf{UnityGfx*}=5-6
com.tencent.tmgp.cf{Thread-*}=2-6
com.tencent.tmgp.cf{Job.worker*}=2-4
com.tencent.tmgp.cf{Audio*}=2-4
com.tencent.tmgp.cf=2-6

# 永劫无间手游
com.netease.l22{UnityMain*}=7
com.netease.l22{UnityGfx*}=2-4
com.netease.l22{Thread-*}=2-6
com.netease.l22{Job.worker*}=2-4
com.netease.l22{Audio*}=2-4
com.netease.l22=2-6

# 守塔不能停
com.jwestern.m4d{UnityMain*}=7
com.jwestern.m4d{UnityGfx*}=2-4
com.jwestern.m4d{Thread-*}=2-6
com.jwestern.m4d{Job.worker*}=2-4
com.jwestern.m4d{Audio*}=2-4
com.jwestern.m4d=2-6

# 炉石传说国际服
com.hearthstone{UnityMain*}=7
com.hearthstone{UnityGfx*}=2-4
com.hearthstone{Thread-*}=2-6
com.hearthstone{Job.worker*}=2-4
com.hearthstone{Audio*}=2-4
com.hearthstone=2-6

# 阴阳师
com.netease.onmyoji{UnityMain*}=7
com.netease.onmyoji{UnityGfx*}=2-4
com.netease.onmyoji{Thread-*}=2-6
com.netease.onmyoji{Job.worker*}=2-4
com.netease.onmyoji{Audio*}=2-4
com.netease.onmyoji=2-6

# 崩坏3
com.miHoYo.bh3.uc{UnityMain*}=7
com.miHoYo.bh3.uc{UnityGfx*}=2-4
com.miHoYo.bh3.uc{Thread-*}=2-6
com.miHoYo.bh3.uc{Job.worker*}=2-4
com.miHoYo.bh3.uc{Audio*}=2-4
com.miHoYo.bh3.uc=2-6

# 崩坏3
com.miHoYo.bh3.mi{UnityMain*}=7
com.miHoYo.bh3.mi{UnityGfx*}=2-4
com.miHoYo.bh3.mi{Thread-*}=2-6
com.miHoYo.bh3.mi{Job.worker*}=2-4
com.miHoYo.bh3.mi{Audio*}=2-4
com.miHoYo.bh3.mi=2-6

# 崩坏3
com.miHoYo.enterprise.NGHSoD{UnityMain*}=7
com.miHoYo.enterprise.NGHSoD{UnityGfx*}=2-4
com.miHoYo.enterprise.NGHSoD{Thread-*}=2-6
com.miHoYo.enterprise.NGHSoD{Job.worker*}=2-4
com.miHoYo.enterprise.NGHSoD{Audio*}=2-4
com.miHoYo.enterprise.NGHSoD=2-6

# 崩坏3
com.miHoYo.bh3.bilibili{UnityMain*}=7
com.miHoYo.bh3.bilibili{UnityGfx*}=2-4
com.miHoYo.bh3.bilibili{Thread-*}=2-6
com.miHoYo.bh3.bilibili{Job.worker*}=2-4
com.miHoYo.bh3.bilibili{Audio*}=2-4
com.miHoYo.bh3.bilibili=2-6

# 明日方舟
com.hypergryph.arknights{UnityMain*}=7
com.hypergryph.arknights{UnityGfx*}=2-4
com.hypergryph.arknights{Thread-*}=2-6
com.hypergryph.arknights{Job.worker*}=2-4
com.hypergryph.arknights{Audio*}=2-4
com.hypergryph.arknights=2-6

# 星球重启
com.hermes.j1game.mi{UnityMain*}=7
com.hermes.j1game.mi{UnityGfx*}=2-4
com.hermes.j1game.mi{Thread-*}=2-6
com.hermes.j1game.mi{Job.worker*}=2-4
com.hermes.j1game.mi{Audio*}=2-4
com.hermes.j1game.mi=2-6

# 星球重启
com.hermes.j1game{UnityMain*}=7
com.hermes.j1game{UnityGfx*}=2-4
com.hermes.j1game{Thread-*}=2-6
com.hermes.j1game{Job.worker*}=2-4
com.hermes.j1game{Audio*}=2-4
com.hermes.j1game=2-6

# 星球重启
com.hermes.j1game.aligames{UnityMain*}=7
com.hermes.j1game.aligames{UnityGfx*}=2-4
com.hermes.j1game.aligames{Thread-*}=2-6
com.hermes.j1game.aligames{Job.worker*}=2-4
com.hermes.j1game.aligames{Audio*}=2-4
com.hermes.j1game.aligames=2-6

# 星球重启
com.hermes.j1game.huawei{UnityMain*}=7
com.hermes.j1game.huawei{UnityGfx*}=2-4
com.hermes.j1game.huawei{Thread-*}=2-6
com.hermes.j1game.huawei{Job.worker*}=2-4
com.hermes.j1game.huawei{Audio*}=2-4
com.hermes.j1game.huawei=2-6

# 星球重启
com.hermes.j1game.vivo{UnityMain*}=7
com.hermes.j1game.vivo{UnityGfx*}=2-4
com.hermes.j1game.vivo{Thread-*}=2-6
com.hermes.j1game.vivo{Job.worker*}=2-4
com.hermes.j1game.vivo{Audio*}=2-4
com.hermes.j1game.vivo=2-6

# 地下城与勇士手游
com.tencent.tmgp.dnf{UnityMain*}=7
com.tencent.tmgp.dnf{UnityGfx*}=2-4
com.tencent.tmgp.dnf{Thread-*}=2-6
com.tencent.tmgp.dnf{Job.worker*}=2-4
com.tencent.tmgp.dnf{Audio*}=2-4
com.tencent.tmgp.dnf=2-6

# 逆水寒手游
com.netease.nshm{UnityMain*}=7
com.netease.nshm{UnityGfx*}=2-4
com.netease.nshm{Thread-*}=2-6
com.netease.nshm{Job.worker*}=2-4
com.netease.nshm{Audio*}=2-4
com.netease.nshm=2-6

# 学园偶像大师
com.bandainamcoent.idolmaster_gakuen{UnityMain*}=7
com.bandainamcoent.idolmaster_gakuen{UnityGfx*}=2-4
com.bandainamcoent.idolmaster_gakuen{Thread-*}=2-6
com.bandainamcoent.idolmaster_gakuen{Job.worker*}=2-4
com.bbandainamcoent.idolmaster_gakuen{Audio*}=2-4
com.bandainamcoent.idolmaster_gakuen=2-6

# 猎魂觉醒网易
com.netease.AVALON{UnityMain*}=7
com.netease.AVALON{UnityGfx*}=2-4
com.netease.AVALON{Thread-*}=2-6
com.netease.AVALON{Job.worker*}=2-4
com.netease.AVALON{Audio*}=2-4
com.netease.AVALON=2-6

# 深空之眼
com.yongshi.tenojo.ys{UnityMain*}=7
com.yongshi.tenojo.ys{UnityGfx*}=2-4
com.yongshi.tenojo.ys{Thread-*}=2-6
com.yongshi.tenojo.ys{Job.worker*}=2-4
com.yongshi.tenojo.ys{Audio*}=2-4
com.yongshi.tenojo.ys=2-6

# 部落冲突
com.tencent.tmgp.supercell.clashofclans{UnityMain*}=7
com.tencent.tmgp.supercell.clashofclans{UnityGfx*}=2-4
com.tencent.tmgp.supercell.clashofclans{Thread-*}=2-6
com.tencent.tmgp.supercell.clashofclans{Job.worker*}=2-4
com.tencent.tmgp.supercell.clashofclans{Audio*}=2-4
com.tencent.tmgp.supercell.clashofclans=2-6

# 海岛奇兵国际服
com.supercell.boombeach{UnityMain*}=7
com.supercell.boombeach{UnityGfx*}=2-4
com.supercell.boombeach{Thread-*}=2-6
com.supercell.boombeach{Job.worker*}=2-4
com.supercell.boombeach{Audio*}=2-4
com.supercell.boombeach=2-6

# 幻塔
com.pwrd.hotta.laohu{UnityMain*}=7
com.pwrd.hotta.laohu{UnityGfx*}=2-4
com.pwrd.hotta.laohu{Thread-*}=2-6
com.pwrd.hotta.laohu{Job.worker*}=2-4
com.pwrd.hotta.laohu{Audio*}=2-4
com.pwrd.hotta.laohu=2-6

# 火影忍者
com.tencent.KiHan{UnityMain*}=7
com.tencent.KiHan{UnityGfx*}=2-4
com.tencent.KiHan{Thread-*}=2-6
com.tencent.KiHan{Job.worker*}=2-4
com.tencent.KiHan{Audio*}=2-4
com.tencent.KiHan=2-6

# 金铲铲之战
com.tencent.jkchess{UnityMain*}=7
com.tencent.jkchess{UnityGfx*}=2-4
com.tencent.jkchess{Thread-*}=2-6
com.tencent.jkchess{Job.worker*}=2-4
com.tencent.jkchess{Audio*}=2-4
com.tencent.jkchess=2-6

# 使命召唤手游
com.tencent.tmgp.cod{UnityMain*}=7
com.tencent.tmgp.cod{UnityGfx*}=2-4
com.tencent.tmgp.cod{Thread-*}=2-6
com.tencent.tmgp.cod{Job.worker*}=2-4
com.tencent.tmgp.cod{Audio*}=2-4
com.tencent.tmgp.cod=2-6

# 碧蓝航线
com.bilibili.azurlane{UnityMain*}=7
com.bilibili.azurlane{UnityGfx*}=2-4
com.bilibili.azurlane{Thread-*}=2-6
com.bilibili.azurlane{Job.worker*}=2-4
com.bilibili.azurlane{Audio*}=2-4
com.bilibili.azurlane=2-6

# 战双帕弥什
com.kurogame.haru.aligames{UnityMain*}=7
com.kurogame.haru.aligames{UnityGfx*}=2-4
com.kurogame.haru.aligames{Thread-*}=2-6
com.kurogame.haru.aligames{Job.worker*}=2-4
com.kurogame.haru.aligames{Audio*}=2-4
com.kurogame.haru.aligames=2-6

# 战双帕弥什
com.kurogame.haru.hero{UnityMain*}=7
com.kurogame.haru.hero{UnityGfx*}=2-4
com.kurogame.haru.hero{Thread-*}=2-6
com.kurogame.haru.hero{Job.worker*}=2-4
com.kurogame.haru.hero{Audio*}=2-4
com.kurogame.haru.hero=2-6

# 来自星尘
com.hypergryph.exastris{UnityMain*}=7
com.hypergryph.exastris{UnityGfx*}=2-4
com.hypergryph.exastris{Thread-*}=2-6
com.hypergryph.exastris{Job.worker*}=2-4
com.hypergryph.exastris{Audio*}=2-4
com.hypergryph.exastris=2-6

# 重返未来1999
com.shenlan.m.reverse1999{UnityMain*}=7
com.shenlan.m.reverse1999{UnityGfx*}=2-4
com.shenlan.m.reverse1999{Thread-*}=2-6
com.shenlan.m.reverse1999{Job.worker*}=2-4
com.shenlan.m.reverse1999{Audio*}=2-4
com.shenlan.m.reverse1999=2-6

# 蛋仔派对
com.netease.party.nearme.gamecenter{UnityMain*}=7
com.netease.party.nearme.gamecenter{UnityGfx*}=2-4
com.netease.party.nearme.gamecenter{Thread-*}=2-6
com.netease.party.nearme.gamecenter{Job.worker*}=2-4
com.netease.party.nearme.gamecenter{Audio*}=2-4
com.netease.party.nearme.gamecenter=2-6

# 蛋仔派对
com.netease.party{UnityMain*}=7
com.netease.party{UnityGfx*}=2-4
com.netease.party{Thread-*}=2-6
com.netease.party{Job.worker*}=2-4
com.netease.party{Audio*}=2-4
com.netease.party=2-6

# 元梦之星
com.tencent.letsgo{UnityMain*}=7
com.tencent.letsgo{UnityGfx*}=2-4
com.tencent.letsgo{Thread-*}=2-6
com.tencent.letsgo{Job.worker*}=2-4
com.tencent.letsgo{Audio*}=2-4
com.tencent.letsgo=2-6

# NBA 2K20
com.t2ksports.nba2k20and{UnityMain*}=7
com.t2ksports.nba2k20and{UnityGfx*}=2-4
com.t2ksports.nba2k20and{Thread-*}=2-6
com.t2ksports.nba2k20and{Job.worker*}=2-4
com.t2ksports.nba2k20and{Audio*}=2-4
com.t2ksports.nba2k20and=2-6

# 王牌竞速
com.netease.aceracer.aligames{UnityMain*}=7
com.netease.aceracer.aligames{UnityGfx*}=2-4
com.netease.aceracer.aligames{Thread-*}=2-6
com.netease.aceracer.aligames{Job.worker*}=2-4
com.netease.aceracer.aligames{Audio*}=2-4
com.netease.aceracer.aligames=2-6

# 香肠派对
com.sofunny.Sausage{UnityMain*}=7
com.sofunny.Sausage{UnityGfx*}=2-4
com.sofunny.Sausage{Thread-*}=2-6
com.sofunny.Sausage{Job.worker*}=2-4
com.sofunny.Sausage{Audio*}=2-4
com.sofunny.Sausage=2-6

# 迷你世界
com.playmini.miniworld{UnityMain*}=7
com.playmini.miniworld{UnityGfx*}=2-4
com.playmini.miniworld{Thread-*}=2-6
com.playmini.miniworld{Job.worker*}=2-4
com.playmini.miniworld{Audio*}=2-4
com.playmini.miniworld=2-6

# 尘白禁区
com.dragonli.projectsnow.lhm{UnityMain*}=7
com.dragonli.projectsnow.lhm{UnityGfx*}=2-4
com.dragonli.projectsnow.lhm{Thread-*}=2-6
com.dragonli.projectsnow.lhm{Job.worker*}=2-4
com.dragonli.projectsnow.lhm{Audio*}=2-4
com.dragonli.projectsnow.lhm=2-6

# 绝区零
com.miHoYo.Nap{UnityMain*}=7
com.miHoYo.Nap{UnityGfx*}=2-4
com.miHoYo.Nap{Thread-*}=2-6
com.miHoYo.Nap{Job.worker*}=2-4
com.miHoYo.Nap{Audio*}=2-4
com.miHoYo.Nap=2-6

# 使命召唤:战争地带
com.activision.callofduty.warzone{UnityMain*}=7
com.activision.callofduty.warzone{UnityGfx*}=2-4
com.activision.callofduty.warzone{Thread-*}=2-6
com.activision.callofduty.warzone{Job.worker*}=2-4
com.activision.callofduty.warzone{Audio*}=2-4
com.activision.callofduty.warzone{*rThread*}=2-6
com.activision.callofduty.warzone{GameThread*}=7
com.activision.callofduty.warzone=2-6

# YGOPro3
com.YGO.MDPro3{UnityMain*}=7
com.YGO.MDPro3{UnityGfx*}=2-4
com.YGO.MDPro3{Thread-*}=2-6
com.YGO.MDPro3{Job.worker*}=2-4
com.YGO.MDPro3{Audio*}=2-4
com.YGO.MDPro3=2-6

# 和平精英日韩服
com.pubg.krmobile{Thread-*}=7
com.pubg.krmobile{*rThread*}=2-6
com.pubg.krmobile{Audio*}=2-4
com.pubg.krmobile=2-4

# 和平精英国际服
com.tencent.ig{Thread-*}=7
com.tencent.ig{*rThread*}=2-6
com.tencent.ig{Audio*}=2-4
com.tencent.ig=2-4

# 鸣潮
com.kurogame.mingchao{Thread-*}=7
com.kurogame.mingchao{*rThread*}=2-6
com.kurogame.mingchao{GameThread*}=7
com.kurogame.mingchao{Audio*}=2-4
com.kurogame.mingchao=2-4

# 晶核
com.hermes.p6game{Thread-*}=7
com.hermes.p6game{*rThread*}=2-6
com.hermes.p6game{GameThread*}=7
com.hermes.p6game{Audio*}=2-4
com.hermes.p6game=2-4

# 晶核
com.hermes.p6game.aligames{Thread-*}=7
com.hermes.p6game.aligames{*rThread*}=2-6
com.hermes.p6game.aligames{GameThread*}=7
com.hermes.p6game.aligames{Audio*}=2-4
com.hermes.p6game.aligames=2-4

# 晶核
com.hermes.p6game.huawei{Thread-*}=7
com.hermes.p6game.huawei{*rThread*}=2-6
com.hermes.p6game.huawei{GameThread*}=7
com.hermes.p6game.huawei{Audio*}=2-4
com.hermes.p6game.huawei=2-4

# 暗区突围
com.tencent.mf.uam{Thread-*}=7
com.tencent.mf.uam{*rThread*}=2-6
com.tencent.mf.uam{GameThread*}=7
com.tencent.mf.uam{Audio*}=2-4
com.tencent.mf.uam=2-4

# 巅峰极速
com.netease.race{Thread-*}=7
com.netease.race{*rThread*}=2-6
com.netease.race{GameThread*}=7
com.netease.race{Audio*}=2-4
com.netease.race=2-4

# 荒野行动
com.netease.hyxd.nearme.gamecenter{Thread-*}=7
com.netease.hyxd.nearme.gamecenter{*rThread*}=2-6
com.netease.hyxd.nearme.gamecenter{GameThread*}=7
com.netease.hyxd.nearme.gamecenter{Audio*}=2-4
com.netease.hyxd.nearme.gamecenter=2-4

# 荒野行动
com.netease.hyxd.aligames{Thread-*}=7
com.netease.hyxd.aligames{*rThread*}=2-6
com.netease.hyxd.aligames{GameThread*}=7
com.netease.hyxd.aligames{Audio*}=2-4
com.netease.hyxd.aligames=2-4

# 三角洲行动
com.tencent.tmgp.dfm{RenderThread*}=7
com.tencent.tmgp.dfm{GameThread*}=2-4,7
com.tencent.tmgp.dfm{Audio*}=2-4
com.tencent.tmgp.dfm=2-6

# 极品飞车:集结
com.tencent.nfsonline{UnityMain*}=7
com.tencent.nfsonline{UnityGfx*}=2-4
com.tencent.nfsonline{Thread-*}=2-6
com.tencent.nfsonline{Job.worker*}=2-4
com.tencent.nfsonline{Audio*}=2-4
com.tencent.nfsonline{*rThread*}=2-6
com.tencent.nfsonline{GameThread*}=7
com.tencent.nfsonline=2-4

# 燕云十六声
com.netease.yyslscn{Thread-*}=7
com.netease.yyslscn{*rThread*}=2-6
com.netease.yyslscn{GameThread*}=7
com.netease.yyslscn{Audio*}=2-4
com.netease.yyslscn=2-4

# Pro象棋
vip.wqby.pro{*Thread*}=2-6
vip.wqby.pro{binder*}=2-6
vip.wqby.pro{Thread-*}=2-6
vip.wqby.pro=5-6

# JJ象棋
cn.jj.chess{UnityMain*}=7
cn.jj.chess{UnityGfx*}=2-4
cn.jj.chess{Thread-*}=2-6
cn.jj.chess{Job.Worker*}=2-4
cn.jj.chess{Audio*}=2-4
cn.jj.chess=2-6

# 棋路
cn.apppk.apps.chessroad{*Thread*}=2-6
cn.apppk.apps.chessroad{binder*}=2-6
cn.apppk.apps.chessroad{Thread-*}=2-6
cn.apppk.apps.chessroad=5-6

# 一念逍遥
com.leiting.xian{UnityMain*}=7
com.leiting.xian{UnityGfx*}=2-4
com.leiting.xian{Thread-*}=2-6
com.leiting.xian{Job.Worker*}=2-4
com.leiting.xian{Audio*}=2-4
com.leiting.xian=2-6

# 狼人杀(网易)
com.netease.lrs{UnityMain*}=7
com.netease.lrs{UnityGfx*}=2-4
com.netease.lrs{Thread-*}=2-6
com.netease.lrs{Job.Worker*}=2-4
com.netease.lrs{Audio*}=2-4
com.netease.lrs=2-6

# 无畏契约
com.tencent.tmgp.codev{GameThread}=7
com.tencent.tmgp.codev{*rThread*}=2-4
com.tencent.tmgp.codev{Audio*}=2-4
com.tencent.tmgp.codev=2-6

# 三国杀
com.sgshd.nearme.gamecenter{*Thread*}=2-6
com.sgshd.nearme.gamecenter{binder*}=2-6
com.sgshd.nearme.gamecenter{Thread-*}=2-6
com.sgshd.nearme.gamecenter{com.sgshd.nearme.gamecenter}=2-6
com.sgshd.nearme.gamecenter=2-4">"$DIR/applist.conf"

echo "* 2+3+2+1线程配置执行完成"