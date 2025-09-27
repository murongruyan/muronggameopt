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

#设置备份目录为上层目录
BACKUP_DIR="$DIR"

#创建备份目录如果它不存在
mkdir -p "$BACKUP_DIR"

#生成备份文件名和时间戳
BACKUP_FILE="$BACKUP_DIR/applist.conf_$(date +%Y%m%d_%H%M%S).bak"

# 检查文件是否在，在进行备份
if [ -f "$FILE_PATH" ]; then
    cp "$FILE_PATH" "$BACKUP_FILE"
    echo "已备份原文件到 $BACKUP_FILE"
fi
	echo "#将QQ音乐主进程绑定到CPU0-4
com.tencent.qqmusic=0-4

#将微信输入法进程绑定到CPU0-4
com.tencent.wetype:play=0-4

#将酷狗音乐后台播放的子进程绑定到CPU0-4
com.kugou.android.support=0-4
com.kugou.android.message=0-4

#将微信Push消息推送进程绑定到小核CPU0-4
com.tencent.mm:push=0-4

#系统桌面
com.android.launcher=0-4
com.android.launcher{RenderThread}=2-4,7
com.android.launcher{binder*}=5-7
com.android.launcher{ndroid.launcher}=5-7

# mt管理器
bin.mt.plus=0-4
bin.mt.plus{RenderThread}=2-4,7
bin.mt.plus{binder*}=5-7
bin.mt.plus{bin.mt.plus}=5-7

# mt管理器共存版
bin.mt.plus.canary=0-4
bin.mt.plus.canary{RenderThread}=2-4,7
bin.mt.plus.canary{binder*}=5-7
bin.mt.plus.canary{.mt.plus.canary}=5-7

# 番茄小说
com.dragon.read=0-4

# 酷安
com.coolapk.market=0-4
com.coolapk.market{RenderThread}=2-4,7
com.coolapk.market{.coolapk.market}=5-7
com.coolapk.market{Thread-*}=5-7
com.coolapk.market{binder:*}=5-7

# 小红书
com.xingin.xhs=0-4
com.xingin.xhs{RenderThread}=2-4,7
com.xingin.xhs{com.xingin.xhs}=5-7
com.xingin.xhs{HeapTaskDaemon}=5-7
com.xingin.xhs{videodec_*}=5-7

# 微博轻享版
com.weico.international=0-4
com.weico.international{RenderThread}=2-4,7
com.weico.international{o.international}=5-7
com.weico.international{glide-source-th}=5-7
com.weico.international{binder:*}=5-7

# 微博
com.sina.weibo=0-4
com.sina.weibo{com.sina.weibo}=5-7
com.sina.weibo{RenderThread}=2-4,7
com.sina.weibo{Thread-*}=5-7

# 闲鱼
com.taobao.idlefish=0-4
com.taobao.idlefish{RenderThread}=2-4,7
com.taobao.idlefish{1.ui}=5-7
com.taobao.idlefish{taobao.idlefish}=5-7
com.taobao.idlefish{1.raster}=5-7

# 淘宝
com.taobao.taobao=0-4
com.taobao.taobao{WeexJSBridgeTh}=2-4,7
com.taobao.taobao{HeapTaskDaemon}=5-7
com.taobao.taobao{m.taobao.taobao}=2-4,7
com.taobao.taobao{8RYPVI8EZKhJUU}=5-7

# 京东
com.jingdong.app.mall=0-4
com.jingdong.app.mall{RunnerWrapper_8}=5-7
com.jingdong.app.mall{ngdong.app.mall}=2-4,7
com.jingdong.app.mall{pool-15-thread-}=5-7
com.jingdong.app.mall{JDFileDownloade}=5-7
com.jingdong.app.mall{RenderThread}=2-4,7

# 拼多多
com.xunmeng.pinduoduo=0-4
com.xunmeng.pinduoduo{Chat#Single-Syn}=5-7
com.xunmeng.pinduoduo{Startup#RTDispa}=2-4,7
com.xunmeng.pinduoduo{nmeng.pinduoduo}=5-7
com.xunmeng.pinduoduo{RenderThread}=2-4,7
com.xunmeng.pinduoduo{Jit thread pool}=5-7

# 今日头条
com.ss.android.article.news=0-4
com.ss.android.article.news{platform-single}=5-7
com.ss.android.article.news{id.article.news}=5-7
com.ss.android.article.news{RenderThread}=2-4,7
com.ss.android.article.news{MediaCodec_*}=5-7

# 知乎
com.zhihu.android=0-4
com.zhihu.android{m.zhihu.android}=5-7
com.zhihu.android{RenderThread}=2-4,7

# 钉钉
com.alibaba.android.rimet=0-4
com.alibaba.android.rimet={a.android.rimet}=5-7
com.alibaba.android.rimet={RenderThread}=2-4,7
com.alibaba.android.rimet={Doraemon-Proces}=5-7

# 高德地图
com.autonavi.minimap=0-4
com.autonavi.minimap{AJXBizCheck}=5-7
com.autonavi.minimap{JavaScriptThrea}=2-4,7
com.autonavi.minimap{Map-Logical-0}=2-4,7
com.autonavi.minimap{utonavi.minimap}=5-7

# 百度地图
com.baidu.BaiduMap=0-4
com.baidu.BaiduMap{31.1_0223536945}=5-7
com.baidu.BaiduMap{.31.1_062565145}=5-7
com.baidu.BaiduMap{.baidu.BaiduMap}=5-7
com.baidu.BaiduMap{RenderThread}=2-4,7

# 哔哩哔哩
tv.danmaku.bili=0-4
tv.danmaku.bili{tv.danmaku.bili}=5-7
tv.danmaku.bili{RenderThread}=2-4,7
tv.danmaku.bili{Thread-*}=5-7
tv.danmaku.bili{IJK_External_Re}=5-7

# 抖音
com.ss.android.ugc.aweme=0-4
com.ss.android.ugc.aweme{RenderThread}=2-4,7
com.ss.android.ugc.aweme{VDecod2*}=2-4,7
com.ss.android.ugc.aweme{droid.ugc.aweme}=5-7
com.ss.android.ugc.aweme{HeapTaskDaemon}=5-7
com.ss.android.ugc.aweme{#pty-wqp-*}=5-7

# 快手
com.smile.gifmaker=0-4
com.smile.gifmaker{RenderThread}=2-4,7
com.smile.gifmaker{smile.gifmaker}=5-7
com.smile.gifmaker{MediaCodec_*}=5-7

# 微信
com.tencent.mm=0-4
com.tencent.mm{com.tencent.mm}=2-4,7
com.tencent.mm{default_matrix_}=5-7
com.tencent.mm{binder:*}=2-4,7

# 支付宝
com.eg.android.AlipayGphone=0-4
com.eg.android.AlipayGphone{crv-worker-thre}=5-7
com.eg.android.AlipayGphone{id.AlipayGphone}=5-7
com.eg.android.AlipayGphone{RenderThread}=2-4,7

# QQ
com.tencent.mobileqq=0-4
com.tencent.mobileqq{encent.mobileqq}=5-7
com.tencent.mobileqq{RenderThread}=2-4,7
com.tencent.mobileqq{MediaCodec_loop}=5-7

# 王者荣耀
com.tencent.tmgp.sgame{UnityMain*}=7
com.tencent.tmgp.sgame{UnityGfx*}=3-6
com.tencent.tmgp.sgame{Job.worker*}=3-6
com.tencent.tmgp.sgame{Thread*}=3-6
com.tencent.tmgp.sgame{Audio*}=0-2
com.tencent.tmgp.sgame=3-4

#王者荣耀国际版
com.levelinfinite.sgameGlobal.midaspay{UnityMain}=7
com.levelinfinite.sgameGlobal.midaspay{UnityGfxDeviceW}=3-6
com.levelinfinite.sgameGlobal.midaspay{Thread-*}=3-6
com.levelinfinite.sgameGlobal.midaspay{Job.worker*}=3-6
com.levelinfinite.sgameGlobal.midaspay{Audio*}=0-2
com.levelinfinite.sgameGlobal.midaspay=3-4

#王者荣耀国际版
com.levelinfinite.sgameGlobal{UnityMain}=7
com.levelinfinite.sgameGlobal{UnityGfxDeviceW}=3-6
com.levelinfinite.sgameGlobal{Thread-*}=3-6
com.levelinfinite.sgameGlobal{Job.worker*}=3-6
com.levelinfinite.sgameGlobal{Audio*}=0-2
com.levelinfinite.sgameGlobal=3-4

#球球大作战
com.ztgame.bob{UnityMain}=7
com.ztgame.bob{UnityGxDevie}=3-6
com.ztgame.bob{com.ztgame.bob}=3-6
com.ztgame.bob{Thread-*}=3-6
com.ztgame.bob{Audio*}=0-2
com.ztgame.bob=3-4

#跑跑卡丁车
com.tencent.tmgp.WePop{UnityMain}=7
com.tencent.tmgp.WePop{Thread-*}=3-6
com.tencent.tmgp.WePop{UnityChoreograp}=3-6
com.tencent.tmgp.WePop{AsyncReadManage}=3-6
com.tencent.tmgp.WePop{Job.Worker*}=3-6
com.tencent.tmgp.WePop{Audio*}=0-2
com.tencent.tmgp.WePop=3-4

#元气骑士
com.ChillyRoom.DungeonShooter{Thread 0x0xb400}=7
com.ChillyRoom.DungeonShooter{UnityMain}=7
com.ChillyRoom.DungeonShooter{UnityGfxDeviceW}=3-6
com.ChillyRoom.DungeonShooter{Audio*}=0-2
com.ChillyRoom.DungeonShooter{Job.Worker*}=3-6
com.ChillyRoom.DungeonShooter=3-4

#少女前线
com.Sunborn.SnqxExilium{UnityMain}=7
com.Sunborn.SnqxExilium{UnityGfxDeviceW}=3-6
com.Sunborn.SnqxExilium{Job.Worker*}=3-6
com.Sunborn.SnqxExilium{Thread*}=3-6
com.Sunborn.SnqxExilium{Audio*}=0-2
com.Sunborn.SnqxExilium=3-4

#第五人格
com.netease.dwrg.nearme.gamecenter{Thread-*}=7
com.netease.dwrg.nearme.gamecenter{HeapTaskDaemon}=3-6
com.netease.dwrg.nearme.gamecenter{NativeThread}=3-6
com.netease.dwrg.nearme.gamecenter{arme.gamecenter}=3-6
com.netease.dwrg.nearme.gamecenter{Audio*}=0-2
com.netease.dwrg.nearme.gamecenter=3-4

# 原神
com.miHoYo.Yuanshen{UnityMain*}=7
com.miHoYo.Yuanshen{UnityGfx*}=3-6
com.miHoYo.Yuanshen{Thread-*}=3-6
com.miHoYo.Yuanshen{Job.worker*}=3-6
com.miHoYo.Yuanshen{Audio*}=0-2
com.miHoYo.Yuanshen=3-4

# 星穹铁道
com.miHoYo.hkrpg{UnityMain*}=7
com.miHoYo.hkrpg{UnityGfx*}=3-6
com.miHoYo.hkrpg{Thread-*}=3-6
com.miHoYo.hkrpg{Job.worker*}=3-6
com.miHoYo.hkrpg{Audio*}=0-2
com.miHoYo.hkrpg=3-4

# 和平精英
com.tencent.tmgp.pubgmhd{Thread-[0-9]}=7
com.tencent.tmgp.pubgmhd{Thread-[1-2][0-9]}=7
com.tencent.tmgp.pubgmhd{RenderThread*}=3-6
com.tencent.tmgp.pubgmhd{RHIThread*}=3-6
com.tencent.tmgp.pubgmhd{Audio*}=0-2
com.tencent.tmgp.pubgmhd=3-4

# 英雄联盟手游
com.tencent.lolm{UnityMain*}=7
com.tencent.lolm{LogicThread*}=3-6
com.tencent.lolm{Thread-*}=3-6
com.tencent.lolm{Job.worker*}=3-6
com.tencent.lolm{Audio*}=0-2
com.tencent.lolm=3-4

# QQ飞车
com.tencent.tmgp.speedmobile{UnityMain*}=7
com.tencent.tmgp.speedmobile{UnityGfx*}=3-6
com.tencent.tmgp.speedmobile{Job.worker*}=3-6
com.tencent.tmgp.speedmobile{Thread-*}=3-6
com.tencent.tmgp.speedmobile{Audio*}=0-2
com.tencent.tmgp.speedmobile=3-4

# 穿越火线：枪战王者
com.tencent.tmgp.cf{UnityMain*}=7
com.tencent.tmgp.cf{UnityGfx*}=3-6
com.tencent.tmgp.cf{Thread-*}=3-6
com.tencent.tmgp.cf{Job.worker*}=3-6
com.tencent.tmgp.cf{Audio*}=0-2
com.tencent.tmgp.cf=3-4

# 永劫无间手游
com.netease.l22{UnityMain*}=7
com.netease.l22{UnityGfx*}=3-6
com.netease.l22{Thread-*}=3-6
com.netease.l22{Job.worker*}=3-6
com.netease.l22{Audio*}=0-2
com.netease.l22=3-4

# 守塔不能停
com.jwestern.m4d{UnityMain*}=7
com.jwestern.m4d{UnityGfx*}=3-6
com.jwestern.m4d{Thread-*}=3-6
com.jwestern.m4d{Job.worker*}=3-6
com.jwestern.m4d{Audio*}=0-2
com.jwestern.m4d=3-4

# 炉石传说国际服
com.hearthstone{UnityMain*}=7
com.hearthstone{UnityGfx*}=3-6
com.hearthstone{Thread-*}=0-1
com.hearthstone{Job.worker*}=3-6
com.hearthstone{Audio*}=0-2
com.hearthstone=3-4

# 阴阳师
com.netease.onmyoji{UnityMain*}=7
com.netease.onmyoji{UnityGfx*}=3-6
com.netease.onmyoji{Thread-*}=0-1
com.netease.onmyoji{Job.worker*}=3-6
com.netease.onmyoji{Audio*}=0-2
com.netease.onmyoji=3-4

# 崩坏3
com.miHoYo.bh3.uc{UnityMain*}=7
com.miHoYo.bh3.uc{UnityGfx*}=3-6
com.miHoYo.bh3.uc{Thread-*}=3-6
com.miHoYo.bh3.uc{Job.worker*}=3-6
com.miHoYo.bh3.uc{Audio*}=0-2
com.miHoYo.bh3.uc=3-4

# 崩坏3
com.miHoYo.bh3.mi{UnityMain*}=7
com.miHoYo.bh3.mi{UnityGfx*}=3-6
com.miHoYo.bh3.mi{Thread-*}=3-6
com.miHoYo.bh3.mi{Job.worker*}=3-6
com.miHoYo.bh3.mi{Audio*}=0-2
com.miHoYo.bh3.mi=3-4

# 崩坏3
com.miHoYo.enterprise.NGHSoD{UnityMain*}=7
com.miHoYo.enterprise.NGHSoD{UnityGfx*}=3-6
com.miHoYo.enterprise.NGHSoD{Thread-*}=3-6
com.miHoYo.enterprise.NGHSoD{Job.worker*}=3-6
com.miHoYo.enterprise.NGHSoD{Audio*}=0-2
com.miHoYo.enterprise.NGHSoD=3-4

# 崩坏3
com.miHoYo.bh3.bilibili{UnityMain*}=7
com.miHoYo.bh3.bilibili{UnityGfx*}=3-6
com.miHoYo.bh3.bilibili{Thread-*}=3-6
com.miHoYo.bh3.bilibili{Job.worker*}=3-6
com.miHoYo.bh3.bilibili{Audio*}=0-2
com.miHoYo.bh3.bilibili=3-4

# 明日方舟
com.hypergryph.arknights{UnityMain*}=7
com.hypergryph.arknights{UnityGfx*}=3-6
com.hypergryph.arknights{Thread-*}=3-6
com.hypergryph.arknights{Job.worker*}=3-6
com.hypergryph.arknights{Audio*}=0-2
com.hypergryph.arknights=3-4

# 星球重启
com.hermes.j1game.mi{UnityMain*}=7
com.hermes.j1game.mi{UnityGfx*}=3-6
com.hermes.j1game.mi{Thread-*}=3-6
com.hermes.j1game.mi{Job.worker*}=3-6
com.hermes.j1game.mi{Audio*}=0-2
com.hermes.j1game.mi=3-4

# 星球重启
com.hermes.j1game{UnityMain*}=7
com.hermes.j1game{UnityGfx*}=3-6
com.hermes.j1game{Thread-*}=3-6
com.hermes.j1game{Job.worker*}=3-6
com.hermes.j1game{Audio*}=0-2
com.hermes.j1game=3-4

# 星球重启
com.hermes.j1game.aligames{UnityMain*}=7
com.hermes.j1game.aligames{UnityGfx*}=3-6
com.hermes.j1game.aligames{Thread-*}=3-6
com.hermes.j1game.aligames{Job.worker*}=3-6
com.hermes.j1game.aligames{Audio*}=0-2
com.hermes.j1game.aligames=3-4

# 星球重启
com.hermes.j1game.huawei{UnityMain*}=7
com.hermes.j1game.huawei{UnityGfx*}=3-6
com.hermes.j1game.huawei{Thread-*}=3-6
com.hermes.j1game.huawei{Job.worker*}=3-6
com.hermes.j1game.huawei{Audio*}=0-2
com.hermes.j1game.huawei=3-4

# 星球重启
com.hermes.j1game.vivo{UnityMain*}=7
com.hermes.j1game.vivo{UnityGfx*}=3-6
com.hermes.j1game.vivo{Thread-*}=3-6
com.hermes.j1game.vivo{Job.worker*}=3-6
com.hermes.j1game.vivo{Audio*}=0-2
com.hermes.j1game.vivo=3-4

# 地下城与勇士手游
com.tencent.tmgp.dnf{UnityMain*}=7
com.tencent.tmgp.dnf{UnityGfx*}=3-6
com.tencent.tmgp.dnf{Thread-*}=3-6
com.tencent.tmgp.dnf{Job.worker*}=3-6
com.tencent.tmgp.dnf{Audio*}=0-2
com.tencent.tmgp.dnf=3-4

# 逆水寒手游
com.netease.nshm{UnityMain*}=7
com.netease.nshm{UnityGfx*}=3-6
com.netease.nshm{Thread-*}=3-6
com.netease.nshm{Job.worker*}=3-6
com.netease.nshm{Audio*}=0-2
com.netease.nshm=3-4

# 学园偶像大师
com.bandainamcoent.idolmaster_gakuen{UnityMain*}=7
com.bandainamcoent.idolmaster_gakuen{UnityGfx*}=3-6
com.bandainamcoent.idolmaster_gakuen{Thread-*}=3-6
com.bandainamcoent.idolmaster_gakuen{Job.worker*}=3-6
com.bbandainamcoent.idolmaster_gakuen{Audio*}=0-2
com.bandainamcoent.idolmaster_gakuen=3-4

# 猎魂觉醒网易
com.netease.AVALON{UnityMain*}=7
com.netease.AVALON{UnityGfx*}=3-6
com.netease.AVALON{Thread-*}=3-6
com.netease.AVALON{Job.worker*}=3-6
com.netease.AVALON{Audio*}=0-2
com.netease.AVALON=3-4

# 深空之眼
com.yongshi.tenojo.ys{UnityMain*}=7
com.yongshi.tenojo.ys{UnityGfx*}=3-6
com.yongshi.tenojo.ys{Thread-*}=3-6
com.yongshi.tenojo.ys{Job.worker*}=3-6
com.yongshi.tenojo.ys{Audio*}=0-2
com.yongshi.tenojo.ys=3-4

# 部落冲突
com.tencent.tmgp.supercell.clashofclans{UnityMain*}=7
com.tencent.tmgp.supercell.clashofclans{UnityGfx*}=3-6
com.tencent.tmgp.supercell.clashofclans{Thread-*}=3-6
com.tencent.tmgp.supercell.clashofclans{Job.worker*}=3-6
com.tencent.tmgp.supercell.clashofclans{Audio*}=0-2
com.tencent.tmgp.supercell.clashofclans=3-4

# 海岛奇兵国际服
com.supercell.boombeach{UnityMain*}=7
com.supercell.boombeach{UnityGfx*}=3-6
com.supercell.boombeach{Thread-*}=3-6
com.supercell.boombeach{Job.worker*}=3-6
com.supercell.boombeach{Audio*}=0-2
com.supercell.boombeach=3-4

# 幻塔
com.pwrd.hotta.laohu{UnityMain*}=7
com.pwrd.hotta.laohu{UnityGfx*}=3-6
com.pwrd.hotta.laohu{Thread-*}=3-6
com.pwrd.hotta.laohu{Job.worker*}=3-6
com.pwrd.hotta.laohu{Audio*}=0-2
com.pwrd.hotta.laohu=3-4

# 火影忍者
com.tencent.KiHan{UnityMain*}=7
com.tencent.KiHan{UnityGfx*}=3-6
com.tencent.KiHan{Thread-*}=3-6
com.tencent.KiHan{Job.worker*}=3-6
com.tencent.KiHan{Audio*}=0-2
com.tencent.KiHan=3-4

# 金铲铲之战
com.tencent.jkchess{UnityMain*}=7
com.tencent.jkchess{UnityGfx*}=3-6
com.tencent.jkchess{Thread-*}=3-6
com.tencent.jkchess{Job.worker*}=3-6
com.tencent.jkchess{Audio*}=0-2
com.tencent.jkchess=3-4

# 使命召唤手游
com.tencent.tmgp.cod{UnityMain*}=7
com.tencent.tmgp.cod{UnityGfx*}=3-6
com.tencent.tmgp.cod{Thread-*}=3-6
com.tencent.tmgp.cod{Job.worker*}=3-6
com.tencent.tmgp.cod{Audio*}=0-2
com.tencent.tmgp.cod=3-4

# 碧蓝航线
com.bilibili.azurlane{UnityMain*}=7
com.bilibili.azurlane{UnityGfx*}=3-6
com.bilibili.azurlane{Thread-*}=3-6
com.bilibili.azurlane{Job.worker*}=3-6
com.bilibili.azurlane{Audio*}=0-2
com.bilibili.azurlane=3-4

# 战双帕弥什
com.kurogame.haru.aligames{UnityMain*}=7
com.kurogame.haru.aligames{UnityGfx*}=3-6
com.kurogame.haru.aligames{Thread-*}=3-6
com.kurogame.haru.aligames{Job.worker*}=3-6
com.kurogame.haru.aligames{Audio*}=0-2
com.kurogame.haru.aligames=3-4

# 战双帕弥什
com.kurogame.haru.hero{UnityMain*}=7
com.kurogame.haru.hero{UnityGfx*}=3-6
com.kurogame.haru.hero{Thread-*}=3-6
com.kurogame.haru.hero{Job.worker*}=3-6
com.kurogame.haru.hero{Audio*}=0-2
com.kurogame.haru.hero=3-4

# 来自星尘
com.hypergryph.exastris{UnityMain*}=7
com.hypergryph.exastris{UnityGfx*}=3-6
com.hypergryph.exastris{Thread-*}=3-6
com.hypergryph.exastris{Job.worker*}=3-6
com.hypergryph.exastris{Audio*}=0-2
com.hypergryph.exastris=3-4

# 重返未来1999
com.shenlan.m.reverse1999{UnityMain*}=7
com.shenlan.m.reverse1999{UnityGfx*}=3-6
com.shenlan.m.reverse1999{Thread-*}=3-6
com.shenlan.m.reverse1999{Job.worker*}=3-6
com.shenlan.m.reverse1999{Audio*}=0-2
com.shenlan.m.reverse1999=3-4

# 蛋仔派对
com.netease.party.nearme.gamecenter{UnityMain*}=7
com.netease.party.nearme.gamecenter{UnityGfx*}=3-6
com.netease.party.nearme.gamecenter{Thread-*}=3-6
com.netease.party.nearme.gamecenter{Job.worker*}=3-6
com.netease.party.nearme.gamecenter{Audio*}=0-2
com.netease.party.nearme.gamecenter=3-4

# 蛋仔派对
com.netease.party{UnityMain*}=7
com.netease.party{UnityGfx*}=3-6
com.netease.party{Thread-*}=3-6
com.netease.party{Job.worker*}=3-6
com.netease.party{Audio*}=0-2
com.netease.party=3-4

# 元梦之星
com.tencent.letsgo{UnityMain*}=7
com.tencent.letsgo{UnityGfx*}=3-6
com.tencent.letsgo{Thread-*}=3-6
com.tencent.letsgo{Job.worker*}=3-6
com.tencent.letsgo{Audio*}=0-2
com.tencent.letsgo=3-4

# NBA 2K20
com.t2ksports.nba2k20and{UnityMain*}=7
com.t2ksports.nba2k20and{UnityGfx*}=3-6
com.t2ksports.nba2k20and{Thread-*}=3-6
com.t2ksports.nba2k20and{Job.worker*}=3-6
com.t2ksports.nba2k20and{Audio*}=0-2
com.t2ksports.nba2k20and=3-4

# 王牌竞速
com.netease.aceracer.aligames{UnityMain*}=7
com.netease.aceracer.aligames{UnityGfx*}=3-6
com.netease.aceracer.aligames{Thread-*}=3-6
com.netease.aceracer.aligames{Job.worker*}=3-6
com.netease.aceracer.aligames{Audio*}=0-2
com.netease.aceracer.aligames=3-4

# 香肠派对
com.sofunny.Sausage{UnityMain*}=7
com.sofunny.Sausage{UnityGfx*}=3-6
com.sofunny.Sausage{Thread-*}=3-6
com.sofunny.Sausage{Job.worker*}=3-6
com.sofunny.Sausage{Audio*}=0-2
com.sofunny.Sausage=3-4

# 迷你世界
com.playmini.miniworld{UnityMain*}=7
com.playmini.miniworld{UnityGfx*}=3-6
com.playmini.miniworld{Thread-*}=3-6
com.playmini.miniworld{Job.worker*}=3-6
com.playmini.miniworld{Audio*}=0-2
com.playmini.miniworld=3-4

# 尘白禁区
com.dragonli.projectsnow.lhm{UnityMain*}=7
com.dragonli.projectsnow.lhm{UnityGfx*}=3-6
com.dragonli.projectsnow.lhm{Thread-*}=3-6
com.dragonli.projectsnow.lhm{Job.worker*}=3-6
com.dragonli.projectsnow.lhm{Audio*}=0-2
com.dragonli.projectsnow.lhm=3-4

# 绝区零
com.miHoYo.Nap{UnityMain*}=7
com.miHoYo.Nap{UnityGfx*}=3-6
com.miHoYo.Nap{Thread-*}=3-6
com.miHoYo.Nap{Job.worker*}=3-6
com.miHoYo.Nap{Audio*}=0-2
com.miHoYo.Nap=3-4

# 使命召唤:战争地带
com.activision.callofduty.warzone{UnityMain*}=7
com.activision.callofduty.warzone{UnityGfx*}=3-6
com.activision.callofduty.warzone{Thread-*}=3-6
com.activision.callofduty.warzone{Job.worker*}=3-6
com.activision.callofduty.warzone{Audio*}=0-2
com.activision.callofduty.warzone{RenderThread*}=3-6
com.activision.callofduty.warzone{RHIThread*}=3-6
com.activision.callofduty.warzone{MainThread*}=3-6
com.activision.callofduty.warzone{GameThread*}=7
com.activision.callofduty.warzone=3-4

# YGOPro3
com.YGO.MDPro3{UnityMain*}=7
com.YGO.MDPro3{UnityGfx*}=3-6
com.YGO.MDPro3{Thread-*}=3-6
com.YGO.MDPro3{Job.worker*}=3-6
com.YGO.MDPro3{Audio*}=0-2
com.YGO.MDPro3=3-4

# 和平精英日韩服
com.pubg.krmobile{Thread-*}=7
com.pubg.krmobile{RenderThread*}=3-6
com.pubg.krmobile{RHIThread*}=3-6
com.pubg.krmobile{Audio*}=0-2
com.pubg.krmobile=3-4

# 和平精英国际服
com.tencent.ig{Thread-*}=7
com.tencent.ig{RenderThread*}=3-6
com.tencent.ig{RHIThread*}=3-6
com.tencent.ig{Audio*}=0-2
com.tencent.ig=3-4

# 鸣潮
com.kurogame.mingchao{Thread-*}=7
com.kurogame.mingchao{RenderThread*}=3-6
com.kurogame.mingchao{RHIThread*}=3-6
com.kurogame.mingchao{MainThread*}=3-6
com.kurogame.mingchao{GameThread*}=7
com.kurogame.mingchao{Audio*}=0-2
com.kurogame.mingchao=3-4

# 晶核
com.hermes.p6game{Thread-*}=7
com.hermes.p6game{RenderThread*}=3-6
com.hermes.p6game{RHIThread*}=3-6
com.hermes.p6game{MainThread*}=3-6
com.hermes.p6game{GameThread*}=7
com.hermes.p6game{Audio*}=0-2
com.hermes.p6game=3-4

# 晶核
com.hermes.p6game.aligames{Thread-*}=7
com.hermes.p6game.aligames{RenderThread*}=3-6
com.hermes.p6game.aligames{RHIThread*}=3-6
com.hermes.p6game.aligames{MainThread*}=3-6
com.hermes.p6game.aligames{GameThread*}=7
com.hermes.p6game.aligames{Audio*}=0-2
com.hermes.p6game.aligames=3-4

# 晶核
com.hermes.p6game.huawei{Thread-*}=7
com.hermes.p6game.huawei{RenderThread*}=3-6
com.hermes.p6game.huawei{RHIThread*}=3-6
com.hermes.p6game.huawei{MainThread*}=3-6
com.hermes.p6game.huawei{GameThread*}=7
com.hermes.p6game.huawei{Audio*}=0-2
com.hermes.p6game.huawei=3-4

# 暗区突围
com.tencent.mf.uam{Thread-*}=7
com.tencent.mf.uam{RenderThread*}=3-6
com.tencent.mf.uam{RHIThread*}=3-6
com.tencent.mf.uam{MainThread*}=3-6
com.tencent.mf.uam{GameThread*}=7
com.tencent.mf.uam{Audio*}=0-2
com.tencent.mf.uam=3-4

# 巅峰极速
com.netease.race{Thread-*}=7
com.netease.race{RenderThread*}=3-6
com.netease.race{RHIThread*}=3-6
com.netease.race{MainThread*}=3-6
com.netease.race{GameThread*}=7
com.netease.race{Audio*}=0-2
com.netease.race=3-4

# 荒野行动
com.netease.hyxd.nearme.gamecenter{Thread-*}=7
com.netease.hyxd.nearme.gamecenter{RenderThread*}=3-6
com.netease.hyxd.nearme.gamecenter{RHIThread*}=3-6
com.netease.hyxd.nearme.gamecenter{MainThread*}=3-6
com.netease.hyxd.nearme.gamecenter{GameThread*}=7
com.netease.hyxd.nearme.gamecenter{Audio*}=0-2
com.netease.hyxd.nearme.gamecenter=3-4

# 荒野行动
com.netease.hyxd.aligames{Thread-*}=7
com.netease.hyxd.aligames{RenderThread*}=3-6
com.netease.hyxd.aligames{RHIThread*}=3-6
com.netease.hyxd.aligames{MainThread*}=3-6
com.netease.hyxd.aligames{GameThread*}=7
com.netease.hyxd.aligames{Audio*}=0-2
com.netease.hyxd.aligames=3-4

# 三角洲行动
com.tencent.tmgp.dfm{Thread-*}=7
com.tencent.tmgp.dfm{RenderThread*}=3-6
com.tencent.tmgp.dfm{RHIThread*}=3-6
com.tencent.tmgp.dfm{MainThread*}=3-6
com.tencent.tmgp.dfm{GameThread*}=7
com.tencent.tmgp.dfm{Audio*}=0-2
com.tencent.tmgp.dfm=3-4

# 极品飞车:集结
com.tencent.nfsonline{UnityMain*}=7
com.tencent.nfsonline{UnityGfx*}=3-6
com.tencent.nfsonline{Thread-*}=3-6
com.tencent.nfsonline{Job.worker*}=3-6
com.tencent.nfsonline{Audio*}=0-2
com.tencent.nfsonline{RenderThread*}=3-6
com.tencent.nfsonline{RHIThread*}=3-6
com.tencent.nfsonline{MainThread*}=3-6
com.tencent.nfsonline{GameThread*}=7
com.tencent.nfsonline=3-4

# 燕云十六声
com.netease.yyslscn{Thread-*}=7
com.netease.yyslscn{RenderThread*}=3-6
com.netease.yyslscn{RHIThread*}=3-6
com.netease.yyslscn{MainThread*}=3-6
com.netease.yyslscn{GameThread*}=7
com.netease.yyslscn{Audio*}=0-2
com.netease.yyslscn=3-4

# Pro象棋
vip.wqby.pro{RenderThread*}=2-4,7
vip.wqby.pro{binder*}=3-4
vip.wqby.pro{Thread-*}=3-4
vip.wqby.pro{Audio*}=0-2
vip.wqby.pro=3-4

# JJ象棋
cn.jj.chess{UnityMain*}=7
cn.jj.chess{UnityGfx*}=3-4
cn.jj.chess{Thread-*}=3-4
cn.jj.chess{Job.Worker*}=3-4
cn.jj.chess{Audio*}=0-2
cn.jj.chess=3-4

# 棋路
cn.apppk.apps.chessroad{RenderThread*}=3-4
cn.apppk.apps.chessroad{binder*}=7
cn.apppk.apps.chessroad{Thread-*}=3-4
cn.apppk.apps.chessroad{Audio*}=0-2
cn.apppk.apps.chessroad=0-4

# 一念逍遥
com.leiting.xian{UnityMain*}=7
com.leiting.xian{UnityGfx*}=3-4
com.leiting.xian{Thread-*}=3-4
com.leiting.xian{Job.Worker*}=3-4
com.leiting.xian{Audio*}=0-2
com.leiting.xian=3-4

# 狼人杀(网易)
com.netease.lrs{UnityMain*}=7
com.netease.lrs{UnityGfx*}=3-4
com.netease.lrs{Thread-*}=3-4
com.netease.lrs{Job.Worker*}=3-4
com.netease.lrs{Audio*}=0-2
com.netease.lrs=3-4

# 无畏契约
com.tencent.tmgp.codev{GameThread}=7
com.tencent.tmgp.codev{RenderThread}=3-6
com.tencent.tmgp.codev{NativeThread}=3-6
com.tencent.tmgp.codev{Audio*}=0-2
com.tencent.tmgp.codev=3-4">"$DIR/applist.conf"

echo "* 3+4+1线程配置执行完成"