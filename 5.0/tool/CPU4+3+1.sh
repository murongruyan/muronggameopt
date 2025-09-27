#!/system/bin/sh
SCRIPT_DIR=$(dirname "$(realpath "$0")")
DIR=$(dirname "$SCRIPT_DIR")

# 遍历可能的模块目录
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

# 设置备份目录为上层目录
BACKUP_DIR="$DIR"

# 创建备份目录如果它不存在
mkdir -p "$BACKUP_DIR"

# 生成备份文件名和时间戳
BACKUP_FILE="$BACKUP_DIR/applist.conf_$(date +%Y%m%d_%H%M%S).bak"

# 检查原文件是否存在，存在则备份
if [ -f "$FILE_PATH" ]; then
    cp "$FILE_PATH" "$BACKUP_FILE"
    echo "已备份原文件到 $BACKUP_FILE"
fi
	echo "#将QQ音乐主进程绑定0-3
com.tencent.qqmusic=0-3

#将微信输入法进程绑定0-3
com.tencent.wetype:play=0-3

#将酷狗音乐后台播放的子进程绑定0-3
com.kugou.android.support=0-3
com.kugou.android.message=0-3

#将微信Push消息推送进程绑定0-3
com.tencent.mm:push=0-3

#系统桌面
com.android.launcher=0-3
com.android.launcher{*Thread*}=4-6
com.android.launcher{binder*}=4-6
com.android.launcher{ndroid.launcher}=4-6

# mt管理器
bin.mt.plus=0-3

# mt管理器共存版
bin.mt.plus.canary=0-3

# 番茄小说
com.dragon.read=0-3

# 酷安
com.coolapk.market=0-3
com.coolapk.market{.coolapk.market}=4-6
com.coolapk.market{*Thread*}=4-6
com.coolapk.market{binder:*}=4-6

# 小红书
com.xingin.xhs=0-3
com.xingin.xhs{HeapTaskDaemon}=4-6
com.xingin.xhs{videodec_*}=4-6

# 微博轻享版
com.weico.international=0-3
com.weico.international{*Thread*}=4-6
com.weico.international{o.international}=4-6
com.weico.international{glide-source-th}=4-6
com.weico.international{binder:*}=4-6

# 微博
com.sina.weibo=0-3
com.sina.weibo{com.sina.weibo}=4-6
com.sina.weibo{*Thread*}=4-6
com.sina.weibo{Thread-*}=4-6

# 闲鱼
com.taobao.idlefish=0-3
com.taobao.idlefish{*Thread*}=4-6
com.taobao.idlefish{1.ui}=4-6
com.taobao.idlefish{taobao.idlefish}=4-6
com.taobao.idlefish{1.raster}=4-6

# 淘宝
com.taobao.taobao=0-3
com.taobao.taobao{WeexJSBridgeTh}=4-6
com.taobao.taobao{HeapTaskDaemon}=4-6
com.taobao.taobao{m.taobao.taobao}=4-6
com.taobao.taobao{8RYPVI8EZKhJUU}=4-6

# 京东
com.jingdong.app.mall=0-3
com.jingdong.app.mall{RunnerWrapper_8}=4-6
com.jingdong.app.mall{ngdong.app.mall}=4-6
com.jingdong.app.mall{pool-15-thread-}=4-6
com.jingdong.app.mall{JDFileDownloade}=4-6
com.jingdong.app.mall{*Thread*}=4-6

# 拼多多
com.xunmeng.pinduoduo=0-3
com.xunmeng.pinduoduo{Chat#Single-Syn}=4-6
com.xunmeng.pinduoduo{Startup#RTDispa}=4-6
com.xunmeng.pinduoduo{nmeng.pinduoduo}=4-6
com.xunmeng.pinduoduo{*Thread*}=4-6
com.xunmeng.pinduoduo{Jit thread pool}=4-6

# 今日头条
com.ss.android.article.news=0-3
com.ss.android.article.news{platform-single}=4-6
com.ss.android.article.news{id.article.news}=4-6
com.ss.android.article.news{*Thread*}=4-6
com.ss.android.article.news{MediaCodec_*}=4-6

# 知乎
com.zhihu.android=0-3
com.zhihu.android{m.zhihu.android}=4-6
com.zhihu.android{*Thread*}=4-6

# 钉钉
com.alibaba.android.rimet=0-3
com.alibaba.android.rimet={a.android.rimet}=4-6
com.alibaba.android.rimet={*Thread*}=4-6
com.alibaba.android.rimet={Doraemon-Proces}=4-6

# 高德地图
com.autonavi.minimap=0-3
com.autonavi.minimap{AJXBizCheck}=4-6
com.autonavi.minimap{JavaScriptThrea}=4-6
com.autonavi.minimap{Map-Logical-0}=4-6
com.autonavi.minimap{utonavi.minimap}=4-6

# 百度地图
com.baidu.BaiduMap=0-3
com.baidu.BaiduMap{31.1_0223536945}=4-6
com.baidu.BaiduMap{.31.1_062565145}=4-6
com.baidu.BaiduMap{.baidu.BaiduMap}=4-6
com.baidu.BaiduMap{*Thread*}=4-6

# 哔哩哔哩
tv.danmaku.bili=0-3
tv.danmaku.bili{tv.danmaku.bili}=4-6
tv.danmaku.bili{*Thread*}=4-6
tv.danmaku.bili{IJK_External_Re}=4-6

# 抖音
com.ss.android.ugc.aweme=0-3
com.ss.android.ugc.aweme{*Thread*}=4-6
com.ss.android.ugc.aweme{VDecod2*}=4-6
com.ss.android.ugc.aweme{droid.ugc.aweme}=4-6
com.ss.android.ugc.aweme{HeapTaskDaemon}=4-6
com.ss.android.ugc.aweme{#pty-wqp-*}=4-6

# 快手
com.smile.gifmaker=0-3
com.smile.gifmaker{*Thread*}=4-6
com.smile.gifmaker{MediaCodec_*}=4-6

# 微信
com.tencent.mm=0-3
com.tencent.mm{com.tencent.mm}=4-6
com.tencent.mm{default_matrix_}=4-6
com.tencent.mm{binder:*}=4-6

# 支付宝
com.eg.android.AlipayGphone=0-3
com.eg.android.AlipayGphone{crv-worker-thre}=4-6
com.eg.android.AlipayGphone{id.AlipayGphone}=4-6
com.eg.android.AlipayGphone{*Thread*}=4-6

# QQ
com.tencent.mobileqq=0-3
com.tencent.mobileqq{encent.mobileqq}=4-6
com.tencent.mobileqq{*Thread*}=4-6
com.tencent.mobileqq{MediaCodec_loop}=4-6

# 王者荣耀
com.tencent.tmgp.sgame{UnityMain*}=7
com.tencent.tmgp.sgame{UnityGfx*}=4-5
com.tencent.tmgp.sgame{Job.worker*}=4-5
com.tencent.tmgp.sgame{Thread*}=4-6
com.tencent.tmgp.sgame{Audio*}=4-5
com.tencent.tmgp.sgame=4-6

#王者荣耀国际版
com.levelinfinite.sgameGlobal.midaspay{UnityMain}=7
com.levelinfinite.sgameGlobal.midaspay{UnityGfxDeviceW}=4-5
com.levelinfinite.sgameGlobal.midaspay{Thread-*}=4-6
com.levelinfinite.sgameGlobal.midaspay{Job.worker*}=4-5
com.levelinfinite.sgameGlobal.midaspay{Audio*}=4-5
com.levelinfinite.sgameGlobal.midaspay=4-6

#王者荣耀国际版
com.levelinfinite.sgameGlobal{UnityMain}=7
com.levelinfinite.sgameGlobal{UnityGfxDeviceW}=4-5
com.levelinfinite.sgameGlobal{Thread-*}=4-6
com.levelinfinite.sgameGlobal{Job.worker*}=4-5
com.levelinfinite.sgameGlobal{Audio*}=4-5
com.levelinfinite.sgameGlobal=4-6

#球球大作战
com.ztgame.bob{UnityMain}=7
com.ztgame.bob{UnityGxDevie}=4-5
com.ztgame.bob{com.ztgame.bob}=4-5
com.ztgame.bob{Thread-*}=4-6
com.ztgame.bob{Audio*}=4-5
com.ztgame.bob=4-6

#跑跑卡丁车
com.tencent.tmgp.WePop{UnityMain}=7
com.tencent.tmgp.WePop{Thread-*}=4-6
com.tencent.tmgp.WePop{UnityChoreograp}=4-5
com.tencent.tmgp.WePop{AsyncReadManage}=4-5
com.tencent.tmgp.WePop{Job.Worker*}=4-5
com.tencent.tmgp.WePop{Audio*}=4-5
com.tencent.tmgp.WePop=4-6

#元气骑士
com.ChillyRoom.DungeonShooter{Thread 0x0xb400}=7
com.ChillyRoom.DungeonShooter{UnityMain}=7
com.ChillyRoom.DungeonShooter{UnityGfx*}=4-5
com.ChillyRoom.DungeonShooter{Job.Worker*}=4-5
com.ChillyRoom.DungeonShooter{Thread-*}=4-6
com.ChillyRoom.DungeonShooter{Audio*}=4-5
com.ChillyRoom.DungeonShooter=4-6

#少女前线
com.Sunborn.SnqxExilium{UnityMain}=7
com.Sunborn.SnqxExilium{UnityGfx*}=4-5
com.Sunborn.SnqxExilium{Job.Worker*}=4-5
com.Sunborn.SnqxExilium{Thread*}=4-5
com.Sunborn.SnqxExilium{Audio*}=4-5
com.Sunborn.SnqxExilium=4-6

#第五人格
com.netease.dwrg.nearme.gamecenter{Thread-*}=7
com.netease.dwrg.nearme.gamecenter{HeapTaskDaemon}=4-5
com.netease.dwrg.nearme.gamecenter{NativeThread}=4-5
com.netease.dwrg.nearme.gamecenter{arme.gamecenter}=4-5
com.netease.dwrg.nearme.gamecenter{Audio*}=4-5
com.netease.dwrg.nearme.gamecenter=4-6

# 原神
com.miHoYo.Yuanshen{UnityMain*}=7
com.miHoYo.Yuanshen{UnityGfx*}=4-5
com.miHoYo.Yuanshen{Thread-*}=4-6
com.miHoYo.Yuanshen{Job.worker*}=4-5
com.miHoYo.Yuanshen{Audio*}=4-5
com.miHoYo.Yuanshen=4-6

# 星穹铁道
com.miHoYo.hkrpg{UnityMain*}=7
com.miHoYo.hkrpg{UnityGfx*}=4-5
com.miHoYo.hkrpg{Thread-*}=4-6
com.miHoYo.hkrpg{Job.worker*}=4-5
com.miHoYo.hkrpg{Audio*}=4-5
com.miHoYo.hkrpg=4-6

# 和平精英
com.tencent.tmgp.pubgmhd{Thread-[0-9]}=7
com.tencent.tmgp.pubgmhd{Thread-[1-2][0-9]}=7
com.tencent.tmgp.pubgmhd{RenderThread*}=4-6
com.tencent.tmgp.pubgmhd{RHIThread*}=4-6
com.tencent.tmgp.pubgmhd{Audio*}=4-5
com.tencent.tmgp.pubgmhd=4-6

# 英雄联盟手游
com.tencent.lolm{UnityMain*}=7
com.tencent.lolm{LogicThread*}=4-5
com.tencent.lolm{Thread-*}=4-6
com.tencent.lolm{Job.worker*}=4-5
com.tencent.lolm{Audio*}=4-5
com.tencent.lolm=4-6

# QQ飞车
com.tencent.tmgp.speedmobile{UnityMain*}=7
com.tencent.tmgp.speedmobile{UnityGfx*}=4-5
com.tencent.tmgp.speedmobile{Job.worker*}=4-5
com.tencent.tmgp.speedmobile{Thread-*}=4-6
com.tencent.tmgp.speedmobile{Audio*}=4-5
com.tencent.tmgp.speedmobile=4-6

# 穿越火线：枪战王者
com.tencent.tmgp.cf{UnityMain*}=7
com.tencent.tmgp.cf{UnityGfx*}=0-3
com.tencent.tmgp.cf{Thread-*}=4-6
com.tencent.tmgp.cf{Job.worker*}=4-5
com.tencent.tmgp.cf{Audio*}=4-5
com.tencent.tmgp.cf=4-6

# 永劫无间手游
com.netease.l22{UnityMain*}=7
com.netease.l22{UnityGfx*}=4-5
com.netease.l22{Thread-*}=4-6
com.netease.l22{Job.worker*}=4-5
com.netease.l22{Audio*}=4-5
com.netease.l22=4-6

# 守塔不能停
com.jwestern.m4d{UnityMain*}=7
com.jwestern.m4d{UnityGfx*}=4-5
com.jwestern.m4d{Thread-*}=4-6
com.jwestern.m4d{Job.worker*}=4-5
com.jwestern.m4d{Audio*}=4-5
com.jwestern.m4d=4-6

# 炉石传说国际服
com.hearthstone{UnityMain*}=7
com.hearthstone{UnityGfx*}=4-5
com.hearthstone{Thread-*}=4-6
com.hearthstone{Job.worker*}=4-5
com.hearthstone{Audio*}=4-5
com.hearthstone=4-6

# 阴阳师
com.netease.onmyoji{UnityMain*}=7
com.netease.onmyoji{UnityGfx*}=4-5
com.netease.onmyoji{Thread-*}=4-6
com.netease.onmyoji{Job.worker*}=4-5
com.netease.onmyoji{Audio*}=4-5
com.netease.onmyoji=4-6

# 崩坏3
com.miHoYo.bh3.uc{UnityMain*}=7
com.miHoYo.bh3.uc{UnityGfx*}=4-5
com.miHoYo.bh3.uc{Thread-*}=4-6
com.miHoYo.bh3.uc{Job.worker*}=4-5
com.miHoYo.bh3.uc{Audio*}=4-5
com.miHoYo.bh3.uc=4-6

# 崩坏3
com.miHoYo.bh3.mi{UnityMain*}=7
com.miHoYo.bh3.mi{UnityGfx*}=4-5
com.miHoYo.bh3.mi{Thread-*}=4-6
com.miHoYo.bh3.mi{Job.worker*}=4-5
com.miHoYo.bh3.mi{Audio*}=4-5
com.miHoYo.bh3.mi=4-6

# 崩坏3
com.miHoYo.enterprise.NGHSoD{UnityMain*}=7
com.miHoYo.enterprise.NGHSoD{UnityGfx*}=4-5
com.miHoYo.enterprise.NGHSoD{Thread-*}=4-6
com.miHoYo.enterprise.NGHSoD{Job.worker*}=4-5
com.miHoYo.enterprise.NGHSoD{Audio*}=4-5
com.miHoYo.enterprise.NGHSoD=4-6

# 崩坏3
com.miHoYo.bh3.bilibili{UnityMain*}=7
com.miHoYo.bh3.bilibili{UnityGfx*}=4-5
com.miHoYo.bh3.bilibili{Thread-*}=4-6
com.miHoYo.bh3.bilibili{Job.worker*}=4-5
com.miHoYo.bh3.bilibili{Audio*}=4-5
com.miHoYo.bh3.bilibili=4-6

# 明日方舟
com.hypergryph.arknights{UnityMain*}=7
com.hypergryph.arknights{UnityGfx*}=4-5
com.hypergryph.arknights{Thread-*}=4-6
com.hypergryph.arknights{Job.worker*}=4-5
com.hypergryph.arknights{Audio*}=4-5
com.hypergryph.arknights=4-6

# 星球重启
com.hermes.j1game.mi{UnityMain*}=7
com.hermes.j1game.mi{UnityGfx*}=4-5
com.hermes.j1game.mi{Thread-*}=4-6
com.hermes.j1game.mi{Job.worker*}=4-5
com.hermes.j1game.mi{Audio*}=4-5
com.hermes.j1game.mi=4-6

# 星球重启
com.hermes.j1game{UnityMain*}=7
com.hermes.j1game{UnityGfx*}=4-5
com.hermes.j1game{Thread-*}=4-6
com.hermes.j1game{Job.worker*}=4-5
com.hermes.j1game{Audio*}=4-5
com.hermes.j1game=4-6

# 星球重启
com.hermes.j1game.aligames{UnityMain*}=7
com.hermes.j1game.aligames{UnityGfx*}=4-5
com.hermes.j1game.aligames{Thread-*}=4-6
com.hermes.j1game.aligames{Job.worker*}=4-5
com.hermes.j1game.aligames{Audio*}=4-5
com.hermes.j1game.aligames=4-6

# 星球重启
com.hermes.j1game.huawei{UnityMain*}=7
com.hermes.j1game.huawei{UnityGfx*}=4-5
com.hermes.j1game.huawei{Thread-*}=4-6
com.hermes.j1game.huawei{Job.worker*}=4-5
com.hermes.j1game.huawei{Audio*}=4-5
com.hermes.j1game.huawei=4-6

# 星球重启
com.hermes.j1game.vivo{UnityMain*}=7
com.hermes.j1game.vivo{UnityGfx*}=4-5
com.hermes.j1game.vivo{Thread-*}=4-6
com.hermes.j1game.vivo{Job.worker*}=4-5
com.hermes.j1game.vivo{Audio*}=4-5
com.hermes.j1game.vivo=4-6

# 地下城与勇士手游
com.tencent.tmgp.dnf{UnityMain*}=7
com.tencent.tmgp.dnf{UnityGfx*}=4-5
com.tencent.tmgp.dnf{Thread-*}=4-6
com.tencent.tmgp.dnf{Job.worker*}=4-5
com.tencent.tmgp.dnf{Audio*}=4-5
com.tencent.tmgp.dnf=4-6

# 逆水寒手游
com.netease.nshm{UnityMain*}=7
com.netease.nshm{UnityGfx*}=4-5
com.netease.nshm{Thread-*}=4-6
com.netease.nshm{Job.worker*}=4-5
com.netease.nshm{Audio*}=4-5
com.netease.nshm=4-6

# 学园偶像大师
com.bandainamcoent.idolmaster_gakuen{UnityMain*}=7
com.bandainamcoent.idolmaster_gakuen{UnityGfx*}=4-5
com.bandainamcoent.idolmaster_gakuen{Thread-*}=4-6
com.bandainamcoent.idolmaster_gakuen{Job.worker*}=4-5
com.bbandainamcoent.idolmaster_gakuen{Audio*}=4-5
com.bandainamcoent.idolmaster_gakuen=4-6

# 猎魂觉醒网易
com.netease.AVALON{UnityMain*}=7
com.netease.AVALON{UnityGfx*}=4-5
com.netease.AVALON{Thread-*}=4-6
com.netease.AVALON{Job.worker*}=4-5
com.netease.AVALON{Audio*}=4-5
com.netease.AVALON=4-6

# 深空之眼
com.yongshi.tenojo.ys{UnityMain*}=7
com.yongshi.tenojo.ys{UnityGfx*}=4-5
com.yongshi.tenojo.ys{Thread-*}=4-6
com.yongshi.tenojo.ys{Job.worker*}=4-5
com.yongshi.tenojo.ys{Audio*}=4-5
com.yongshi.tenojo.ys=4-6

# 部落冲突
com.tencent.tmgp.supercell.clashofclans{UnityMain*}=7
com.tencent.tmgp.supercell.clashofclans{UnityGfx*}=4-5
com.tencent.tmgp.supercell.clashofclans{Thread-*}=4-6
com.tencent.tmgp.supercell.clashofclans{Job.worker*}=4-5
com.tencent.tmgp.supercell.clashofclans{Audio*}=4-5
com.tencent.tmgp.supercell.clashofclans=4-6

# 海岛奇兵国际服
com.supercell.boombeach{UnityMain*}=7
com.supercell.boombeach{UnityGfx*}=4-5
com.supercell.boombeach{Thread-*}=4-6
com.supercell.boombeach{Job.worker*}=4-5
com.supercell.boombeach{Audio*}=4-5
com.supercell.boombeach=4-6

# 幻塔
com.pwrd.hotta.laohu{UnityMain*}=7
com.pwrd.hotta.laohu{UnityGfx*}=4-5
com.pwrd.hotta.laohu{Thread-*}=4-6
com.pwrd.hotta.laohu{Job.worker*}=4-5
com.pwrd.hotta.laohu{Audio*}=4-5
com.pwrd.hotta.laohu=4-6

# 火影忍者
com.tencent.KiHan{UnityMain*}=7
com.tencent.KiHan{UnityGfx*}=4-5
com.tencent.KiHan{Thread-*}=4-6
com.tencent.KiHan{Job.worker*}=4-5
com.tencent.KiHan{Audio*}=4-5
com.tencent.KiHan=4-6

# 金铲铲之战
com.tencent.jkchess{UnityMain*}=7
com.tencent.jkchess{UnityGfx*}=4-5
com.tencent.jkchess{Thread-*}=4-6
com.tencent.jkchess{Job.worker*}=4-5
com.tencent.jkchess{Audio*}=4-5
com.tencent.jkchess=4-6

# 使命召唤手游
com.tencent.tmgp.cod{UnityMain*}=7
com.tencent.tmgp.cod{UnityGfx*}=4-5
com.tencent.tmgp.cod{Thread-*}=4-6
com.tencent.tmgp.cod{Job.worker*}=4-5
com.tencent.tmgp.cod{Audio*}=4-5
com.tencent.tmgp.cod=4-6

# 碧蓝航线
com.bilibili.azurlane{UnityMain*}=7
com.bilibili.azurlane{UnityGfx*}=4-5
com.bilibili.azurlane{Thread-*}=4-6
com.bilibili.azurlane{Job.worker*}=4-5
com.bilibili.azurlane{Audio*}=4-5
com.bilibili.azurlane=4-6

# 战双帕弥什
com.kurogame.haru.aligames{UnityMain*}=7
com.kurogame.haru.aligames{UnityGfx*}=4-5
com.kurogame.haru.aligames{Thread-*}=4-6
com.kurogame.haru.aligames{Job.worker*}=4-5
com.kurogame.haru.aligames{Audio*}=4-5
com.kurogame.haru.aligames=4-6

# 战双帕弥什
com.kurogame.haru.hero{UnityMain*}=7
com.kurogame.haru.hero{UnityGfx*}=4-5
com.kurogame.haru.hero{Thread-*}=4-6
com.kurogame.haru.hero{Job.worker*}=4-5
com.kurogame.haru.hero{Audio*}=4-5
com.kurogame.haru.hero=4-6

# 来自星尘
com.hypergryph.exastris{UnityMain*}=7
com.hypergryph.exastris{UnityGfx*}=4-5
com.hypergryph.exastris{Thread-*}=4-6
com.hypergryph.exastris{Job.worker*}=4-5
com.hypergryph.exastris{Audio*}=4-5
com.hypergryph.exastris=4-6

# 重返未来1999
com.shenlan.m.reverse1999{UnityMain*}=7
com.shenlan.m.reverse1999{UnityGfx*}=4-5
com.shenlan.m.reverse1999{Thread-*}=4-6
com.shenlan.m.reverse1999{Job.worker*}=4-5
com.shenlan.m.reverse1999{Audio*}=4-5
com.shenlan.m.reverse1999=4-6

# 蛋仔派对
com.netease.party.nearme.gamecenter{UnityMain*}=7
com.netease.party.nearme.gamecenter{UnityGfx*}=4-5
com.netease.party.nearme.gamecenter{Thread-*}=4-6
com.netease.party.nearme.gamecenter{Job.worker*}=4-5
com.netease.party.nearme.gamecenter{Audio*}=4-5
com.netease.party.nearme.gamecenter=4-6

# 蛋仔派对
com.netease.party{UnityMain*}=7
com.netease.party{UnityGfx*}=4-5
com.netease.party{Thread-*}=4-6
com.netease.party{Job.worker*}=4-5
com.netease.party{Audio*}=4-5
com.netease.party=4-6

# 元梦之星
com.tencent.letsgo{UnityMain*}=7
com.tencent.letsgo{UnityGfx*}=4-5
com.tencent.letsgo{Thread-*}=4-6
com.tencent.letsgo{Job.worker*}=4-5
com.tencent.letsgo{Audio*}=4-5
com.tencent.letsgo=4-6

# NBA 2K20
com.t2ksports.nba2k20and{UnityMain*}=7
com.t2ksports.nba2k20and{UnityGfx*}=4-5
com.t2ksports.nba2k20and{Thread-*}=4-6
com.t2ksports.nba2k20and{Job.worker*}=4-5
com.t2ksports.nba2k20and{Audio*}=4-5
com.t2ksports.nba2k20and=4-6

# 王牌竞速
com.netease.aceracer.aligames{UnityMain*}=7
com.netease.aceracer.aligames{UnityGfx*}=4-5
com.netease.aceracer.aligames{Thread-*}=4-6
com.netease.aceracer.aligames{Job.worker*}=4-5
com.netease.aceracer.aligames{Audio*}=4-5
com.netease.aceracer.aligames=4-6

# 香肠派对
com.sofunny.Sausage{UnityMain*}=7
com.sofunny.Sausage{UnityGfx*}=4-5
com.sofunny.Sausage{Thread-*}=4-6
com.sofunny.Sausage{Job.worker*}=4-5
com.sofunny.Sausage{Audio*}=4-5
com.sofunny.Sausage=4-6

# 迷你世界
com.playmini.miniworld{UnityMain*}=7
com.playmini.miniworld{UnityGfx*}=4-5
com.playmini.miniworld{Thread-*}=4-6
com.playmini.miniworld{Job.worker*}=4-5
com.playmini.miniworld{Audio*}=4-5
com.playmini.miniworld=4-6

# 尘白禁区
com.dragonli.projectsnow.lhm{UnityMain*}=7
com.dragonli.projectsnow.lhm{UnityGfx*}=4-5
com.dragonli.projectsnow.lhm{Thread-*}=4-6
com.dragonli.projectsnow.lhm{Job.worker*}=4-5
com.dragonli.projectsnow.lhm{Audio*}=4-5
com.dragonli.projectsnow.lhm=4-6

# 绝区零
com.miHoYo.Nap{UnityMain*}=7
com.miHoYo.Nap{UnityGfx*}=4-5
com.miHoYo.Nap{Thread-*}=4-6
com.miHoYo.Nap{Job.worker*}=4-5
com.miHoYo.Nap{Audio*}=4-5
com.miHoYo.Nap=4-6

# 使命召唤:战争地带
com.activision.callofduty.warzone{UnityMain*}=7
com.activision.callofduty.warzone{UnityGfx*}=4-5
com.activision.callofduty.warzone{Thread-*}=4-6
com.activision.callofduty.warzone{Job.worker*}=4-5
com.activision.callofduty.warzone{Audio*}=4-5
com.activision.callofduty.warzone{RenderThread*}=4-6
com.activision.callofduty.warzone{RHIThread*}=4-6
com.activision.callofduty.warzone{MainThread*}=4-6
com.activision.callofduty.warzone{GameThread*}=7
com.activision.callofduty.warzone=4-6

# YGOPro3
com.YGO.MDPro3{UnityMain*}=7
com.YGO.MDPro3{UnityGfx*}=4-5
com.YGO.MDPro3{Thread-*}=4-6
com.YGO.MDPro3{Job.worker*}=4-5
com.YGO.MDPro3{Audio*}=4-5
com.YGO.MDPro3=4-6

# 和平精英日韩服
com.pubg.krmobile{Thread-*}=7
com.pubg.krmobile{RenderThread*}=4-6
com.pubg.krmobile{RHIThread*}=4-6
com.pubg.krmobile{Audio*}=4-5
com.pubg.krmobile=4-5

# 和平精英国际服
com.tencent.ig{Thread-*}=7
com.tencent.ig{RenderThread*}=4-6
com.tencent.ig{RHIThread*}=4-6
com.tencent.ig{Audio*}=4-5
com.tencent.ig=4-5

# 鸣潮
com.kurogame.mingchao{Thread-*}=7
com.kurogame.mingchao{RenderThread*}=4-6
com.kurogame.mingchao{RHIThread*}=4-6
com.kurogame.mingchao{MainThread*}=4-6
com.kurogame.mingchao{GameThread*}=7
com.kurogame.mingchao{Audio*}=4-5
com.kurogame.mingchao=4-5

# 晶核
com.hermes.p6game{Thread-*}=7
com.hermes.p6game{RenderThread*}=4-6
com.hermes.p6game{RHIThread*}=4-6
com.hermes.p6game{MainThread*}=4-6
com.hermes.p6game{GameThread*}=7
com.hermes.p6game{Audio*}=4-5
com.hermes.p6game=4-5

# 晶核
com.hermes.p6game.aligames{Thread-*}=7
com.hermes.p6game.aligames{RenderThread*}=4-6
com.hermes.p6game.aligames{RHIThread*}=4-6
com.hermes.p6game.aligames{MainThread*}=4-6
com.hermes.p6game.aligames{GameThread*}=7
com.hermes.p6game.aligames{Audio*}=4-5
com.hermes.p6game.aligames=4-5

# 晶核
com.hermes.p6game.huawei{Thread-*}=7
com.hermes.p6game.huawei{RenderThread*}=4-6
com.hermes.p6game.huawei{RHIThread*}=4-6
com.hermes.p6game.huawei{MainThread*}=4-6
com.hermes.p6game.huawei{GameThread*}=7
com.hermes.p6game.huawei{Audio*}=4-5
com.hermes.p6game.huawei=4-5

# 暗区突围
com.tencent.mf.uam{Thread-*}=7
com.tencent.mf.uam{RenderThread*}=4-6
com.tencent.mf.uam{RHIThread*}=4-6
com.tencent.mf.uam{MainThread*}=4-6
com.tencent.mf.uam{GameThread*}=7
com.tencent.mf.uam{Audio*}=4-5
com.tencent.mf.uam=4-5

# 巅峰极速
com.netease.race{Thread-*}=7
com.netease.race{RenderThread*}=4-6
com.netease.race{RHIThread*}=4-6
com.netease.race{MainThread*}=4-6
com.netease.race{GameThread*}=7
com.netease.race{Audio*}=4-5
com.netease.race=4-5

# 荒野行动
com.netease.hyxd.nearme.gamecenter{Thread-*}=7
com.netease.hyxd.nearme.gamecenter{RenderThread*}=4-6
com.netease.hyxd.nearme.gamecenter{RHIThread*}=4-6
com.netease.hyxd.nearme.gamecenter{MainThread*}=4-6
com.netease.hyxd.nearme.gamecenter{GameThread*}=7
com.netease.hyxd.nearme.gamecenter{Audio*}=4-5
com.netease.hyxd.nearme.gamecenter=4-5

# 荒野行动
com.netease.hyxd.aligames{Thread-*}=7
com.netease.hyxd.aligames{RenderThread*}=4-6
com.netease.hyxd.aligames{RHIThread*}=4-6
com.netease.hyxd.aligames{MainThread*}=4-6
com.netease.hyxd.aligames{GameThread*}=7
com.netease.hyxd.aligames{Audio*}=4-5
com.netease.hyxd.aligames=4-5

# 三角洲行动
com.tencent.tmgp.dfm{Thread-*}=4-6
com.tencent.tmgp.dfm{RenderThread*}=7
com.tencent.tmgp.dfm{RHIThread*}=4-5
com.tencent.tmgp.dfm{MainThread*}=4-5
com.tencent.tmgp.dfm{GameThread*}=4-5,7
com.tencent.tmgp.dfm{Audio*}=4-5
com.tencent.tmgp.dfm=4-5

# 极品飞车:集结
com.tencent.nfsonline{UnityMain*}=7
com.tencent.nfsonline{UnityGfx*}=4-5
com.tencent.nfsonline{Thread-*}=4-6
com.tencent.nfsonline{Job.worker*}=4-5
com.tencent.nfsonline{Audio*}=4-5
com.tencent.nfsonline{RenderThread*}=4-6
com.tencent.nfsonline{RHIThread*}=4-6
com.tencent.nfsonline{MainThread*}=4-6
com.tencent.nfsonline{GameThread*}=7
com.tencent.nfsonline=4-5

# 燕云十六声
com.netease.yyslscn{Thread-*}=7
com.netease.yyslscn{RenderThread*}=4-6
com.netease.yyslscn{RHIThread*}=4-6
com.netease.yyslscn{MainThread*}=4-6
com.netease.yyslscn{GameThread*}=7
com.netease.yyslscn{Audio*}=4-5
com.netease.yyslscn=4-5

# Pro象棋
vip.wqby.pro{RenderThread*}=7
vip.wqby.pro{binder*}=4-5
vip.wqby.pro{Thread-*}=4-6
vip.wqby.pro{Audio*}=4-5
vip.wqby.pro=4-6

# JJ象棋
cn.jj.chess{UnityMain*}=7
cn.jj.chess{UnityGfx*}=4-5
cn.jj.chess{Thread-*}=4-6
cn.jj.chess{Job.Worker*}=4-5
cn.jj.chess{Audio*}=4-5
cn.jj.chess=4-6

# 棋路
cn.apppk.apps.chessroad{RenderThread*}=4-5
cn.apppk.apps.chessroad{binder*}=7
cn.apppk.apps.chessroad{Thread-*}=4-6
cn.apppk.apps.chessroad{Audio*}=4-5
cn.apppk.apps.chessroad=4-6

# 一念逍遥
com.leiting.xian{UnityMain*}=7
com.leiting.xian{UnityGfx*}=4-5
com.leiting.xian{Thread-*}=4-6
com.leiting.xian{Job.Worker*}=4-5
com.leiting.xian{Audio*}=4-5
com.leiting.xian=4-6

# 狼人杀(网易)
com.netease.lrs{UnityMain*}=7
com.netease.lrs{UnityGfx*}=4-5
com.netease.lrs{Thread-*}=4-6
com.netease.lrs{Job.Worker*}=4-5
com.netease.lrs{Audio*}=4-5
com.netease.lrs=4-6

# 无畏契约
com.tencent.tmgp.codev{GameThread}=7
com.tencent.tmgp.codev{RenderThread}=4-5
com.tencent.tmgp.codev{NativeThread}=4-5
com.tencent.tmgp.codev{Audio*}=4-5
com.tencent.tmgp.codev=4-6">"$DIR/applist.conf"

echo "* 4+3+1线程配置执行完成"