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

# 生成备份文件名，包含时间戳
BACKUP_FILE="$BACKUP_DIR/applist.conf_$(date +%Y%m%d_%H%M%S).bak"

# 检查原文件是否存在，如果存在则进行备份
if [ -f "$FILE_PATH" ]; then
    cp "$FILE_PATH" "$BACKUP_FILE"
    echo "已备份原文件到 $BACKUP_FILE"
fi
	echo "#将QQ音乐主进程绑定到CPU0-3
com.tencent.qqmusic=0-3

#将微信输入法进程绑定到CPU0-3
com.tencent.wetype:play=0-3

#将酷狗音乐后台播放的子进程绑定到CPU0-3
com.kugou.android.support=0-3
com.kugou.android.message=0-3

#将微信Push消息推送进程绑定到CPU0-3
com.tencent.mm:push=0-3

#系统桌面
com.android.launcher=0-3

# mt管理器
bin.mt.plus=0-3

# mt管理器共存版
bin.mt.plus.canary=0-3

# 番茄小说
com.dragon.read=0-3

# 酷安
com.coolapk.market=0-3

# 小红书
com.xingin.xhs=0-3

# 微博轻享版
com.weico.international=0-3

# 微博
com.sina.weibo=0-3

# 闲鱼
com.taobao.idlefish=0-3

# 淘宝
com.taobao.taobao=0-3

# 京东
com.jingdong.app.mall=0-3

# 拼多多
com.xunmeng.pinduoduo=0-3

# 今日头条
com.ss.android.article.news=0-3

# 知乎
com.zhihu.android=0-3

# 三国杀
com.sgshd.nearme.gamecenter=0-3

# 钉钉
com.alibaba.android.rimet=0-3

# 高德地图
com.autonavi.minimap=0-3

# 百度地图
com.baidu.BaiduMap=0-3

# 哔哩哔哩
tv.danmaku.bili=0-3

# 抖音
com.ss.android.ugc.aweme=0-3

# 快手
com.smile.gifmaker=0-3

# 微信
com.tencent.mm=0-3

# 支付宝
com.eg.android.AlipayGphone=0-3

# QQ
com.tencent.mobileqq=0-3

# 王者荣耀
com.tencent.tmgp.sgame{UnityMain*}=6-7
com.tencent.tmgp.sgame{UnityGfx*}=6-7
com.tencent.tmgp.sgame=0-5

#王者荣耀国际版
com.levelinfinite.sgameGlobal.midaspay{UnityMain}=6-7
com.levelinfinite.sgameGlobal.midaspay{UnityGfxDeviceW}=6-7
com.levelinfinite.sgameGlobal.midaspay=0-5

#王者荣耀国际版
com.levelinfinite.sgameGlobal{UnityMain}=6-7
com.levelinfinite.sgameGlobal{UnityGfxDeviceW}=6-7
com.levelinfinite.sgameGlobal=0-5

#球球大作战
com.ztgame.bob{UnityMain}=6-7
com.ztgame.bob{UnityGxDevie}=6-7
com.ztgame.bob=0-5

#跑跑卡丁车
com.tencent.tmgp.WePop{UnityMain}=6-7
com.tencent.tmgp.WePop{Thread-*}=6-7
com.tencent.tmgp.WePop=0-5

#元气骑士
com.ChillyRoom.DungeonShooter{UnityMain}=6-7
com.ChillyRoom.DungeonShooter{UnityGfxDeviceW}=6-7
com.ChillyRoom.DungeonShooter=0-5

#少女前线
com.Sunborn.SnqxExilium{UnityMain}=6-7
com.Sunborn.SnqxExilium{UnityGfxDeviceW}=6-7
com.Sunborn.SnqxExilium=0-5

#第五人格
com.netease.dwrg.nearme.gamecenter{Thread-*}=6-7
com.netease.dwrg.nearme.gamecenter{HeapTaskDaemon}=6-7
com.netease.dwrg.nearme.gamecenter=0-5

# 原神
com.miHoYo.Yuanshen{UnityMain*}=6-7
com.miHoYo.Yuanshen{UnityGfx*}=6-7
com.miHoYo.Yuanshen=0-5

# 星穹铁道
com.miHoYo.hkrpg{UnityMain*}=6-7
com.miHoYo.hkrpg{UnityGfx*}=6-7
com.miHoYo.hkrpg=0-5

# 和平精英
com.tencent.tmgp.pubgmhd{Thread-[0-9]}=6-7
com.tencent.tmgp.pubgmhd{Thread-[1-2][0-9]}=6-7
com.tencent.tmgp.pubgmhd{RenderThread*}=6-7
com.tencent.tmgp.pubgmhd=0-5

# 英雄联盟手游
com.tencent.lolm{UnityMain*}=6-7
com.tencent.lolm=0-5

# QQ飞车
com.tencent.tmgp.speedmobile{UnityMain*}=6-7
com.tencent.tmgp.speedmobile{UnityGfx*}=6-7
com.tencent.tmgp.speedmobile=0-5

# 穿越火线：枪战王者
com.tencent.tmgp.cf{UnityMain*}=6-7
com.tencent.tmgp.cf{UnityGfx*}=6-7
com.tencent.tmgp.cf=0-5

# 永劫无间手游
com.netease.l22{UnityMain*}=6-7
com.netease.l22{UnityGfx*}=6-7
com.netease.l22=0-5

# 守塔不能停
com.jwestern.m4d{UnityMain*}=6-7
com.jwestern.m4d{UnityGfx*}=6-7
com.jwestern.m4d=0-5

# 炉石传说国际服
com.hearthstone{UnityMain*}=6-7
com.hearthstone{UnityGfx*}=6-7
com.hearthstone=0-5

# 阴阳师
com.netease.onmyoji{UnityMain*}=6-7
com.netease.onmyoji{UnityGfx*}=6-7
com.netease.onmyoji=0-5

# 崩坏3
com.miHoYo.bh3.uc{UnityMain*}=6-7
com.miHoYo.bh3.uc{UnityGfx*}=6-7
com.miHoYo.bh3.uc=0-5

# 崩坏3
com.miHoYo.bh3.mi{UnityMain*}=6-7
com.miHoYo.bh3.mi{UnityGfx*}=6-7
com.miHoYo.bh3.mi=0-5

# 崩坏3
com.miHoYo.enterprise.NGHSoD{UnityMain*}=6-7
com.miHoYo.enterprise.NGHSoD{UnityGfx*}=6-7
com.miHoYo.enterprise.NGHSoD=0-5

# 崩坏3
com.miHoYo.bh3.bilibili{UnityMain*}=6-7
com.miHoYo.bh3.bilibili{UnityGfx*}=6-7
com.miHoYo.bh3.bilibili=0-5

# 明日方舟
com.hypergryph.arknights{UnityMain*}=6-7
com.hypergryph.arknights{UnityGfx*}=6-7
com.hypergryph.arknights=0-5

# 星球重启
com.hermes.j1game.mi{UnityMain*}=6-7
com.hermes.j1game.mi{UnityGfx*}=6-7
com.hermes.j1game.mi=0-5

# 星球重启
com.hermes.j1game{UnityMain*}=6-7
com.hermes.j1game{UnityGfx*}=6-7
com.hermes.j1game=0-5

# 星球重启
com.hermes.j1game.aligames{UnityMain*}=6-7
com.hermes.j1game.aligames{UnityGfx*}=6-7
com.hermes.j1game.aligames=0-5

# 星球重启
com.hermes.j1game.huawei{UnityMain*}=6-7
com.hermes.j1game.huawei{UnityGfx*}=6-7
com.hermes.j1game.huawei=0-5

# 星球重启
com.hermes.j1game.vivo{UnityMain*}=6-7
com.hermes.j1game.vivo{UnityGfx*}=6-7
com.hermes.j1game.vivo=0-5

# 地下城与勇士手游
com.tencent.tmgp.dnf{UnityMain*}=6-7
com.tencent.tmgp.dnf{UnityGfx*}=6-7
com.tencent.tmgp.dnf=0-5

# 逆水寒手游
com.netease.nshm{UnityMain*}=6-7
com.netease.nshm{UnityGfx*}=6-7
com.netease.nshm=0-5

# 学园偶像大师
com.bandainamcoent.idolmaster_gakuen{UnityMain*}=6-7
com.bandainamcoent.idolmaster_gakuen{UnityGfx*}=6-7
com.bandainamcoent.idolmaster_gakuen=0-5

# 猎魂觉醒网易
com.netease.AVALON{UnityMain*}=6-7
com.netease.AVALON{UnityGfx*}=6-7
com.netease.AVALON=0-5

# 深空之眼
com.yongshi.tenojo.ys{UnityMain*}=6-7
com.yongshi.tenojo.ys{UnityGfx*}=6-7
com.yongshi.tenojo.ys=0-5

# 部落冲突
com.tencent.tmgp.supercell.clashofclans{UnityMain*}=6-7
com.tencent.tmgp.supercell.clashofclans{UnityGfx*}=6-7
com.tencent.tmgp.supercell.clashofclans=0-5

# 海岛奇兵国际服
com.supercell.boombeach{UnityMain*}=6-7
com.supercell.boombeach{UnityGfx*}=6-7
com.supercell.boombeach=0-5

# 幻塔
com.pwrd.hotta.laohu{UnityMain*}=6-7
com.pwrd.hotta.laohu{UnityGfx*}=6-7
com.pwrd.hotta.laohu=0-5

# 火影忍者
com.tencent.KiHan{UnityMain*}=6-7
com.tencent.KiHan{UnityGfx*}=6-7
com.tencent.KiHan=0-5

# 金铲铲之战
com.tencent.jkchess{UnityMain*}=6-7
com.tencent.jkchess{UnityGfx*}=6-7
com.tencent.jkchess=0-5

# 使命召唤手游
com.tencent.tmgp.cod{UnityMain*}=6-7
com.tencent.tmgp.cod{UnityGfx*}=6-7
com.tencent.tmgp.cod=0-5

# 碧蓝航线
com.bilibili.azurlane{UnityMain*}=6-7
com.bilibili.azurlane{UnityGfx*}=6-7
com.bilibili.azurlane=0-5

# 战双帕弥什
com.kurogame.haru.aligames{UnityMain*}=6-7
com.kurogame.haru.aligames{UnityGfx*}=6-7
com.kurogame.haru.aligames=0-5

# 战双帕弥什
com.kurogame.haru.hero{UnityMain*}=6-7
com.kurogame.haru.hero{UnityGfx*}=6-7
com.kurogame.haru.hero=0-5

# 来自星尘
com.hypergryph.exastris{UnityMain*}=6-7
com.hypergryph.exastris{UnityGfx*}=6-7
com.hypergryph.exastris=0-5

# 重返未来1999
com.shenlan.m.reverse1999{UnityMain*}=6-7
com.shenlan.m.reverse1999{UnityGfx*}=6-7
com.shenlan.m.reverse1999=0-5

# 蛋仔派对
com.netease.party.nearme.gamecenter{UnityMain*}=6-7
com.netease.party.nearme.gamecenter{UnityGfx*}=6-7
com.netease.party.nearme.gamecenter=0-5

# 蛋仔派对
com.netease.party{UnityMain*}=6-7
com.netease.party{UnityGfx*}=6-7
com.netease.party=0-5

# 元梦之星
com.tencent.letsgo{UnityMain*}=6-7
com.tencent.letsgo{UnityGfx*}=6-7
com.tencent.letsgo=0-5

# NBA 2K20
com.t2ksports.nba2k20and{UnityMain*}=6-7
com.t2ksports.nba2k20and{UnityGfx*}=6-7
com.t2ksports.nba2k20and=0-5

# 王牌竞速
com.netease.aceracer.aligames{UnityMain*}=6-7
com.netease.aceracer.aligames{UnityGfx*}=6-7
com.netease.aceracer.aligames=0-5

# 香肠派对
com.sofunny.Sausage{UnityMain*}=6-7
com.sofunny.Sausage{UnityGfx*}=6-7
com.sofunny.Sausage=0-5

# 迷你世界
com.playmini.miniworld{UnityMain*}=6-7
com.playmini.miniworld{UnityGfx*}=6-7
com.playmini.miniworld=0-5

# 尘白禁区
com.dragonli.projectsnow.lhm{UnityMain*}=6-7
com.dragonli.projectsnow.lhm{UnityGfx*}=6-7
com.dragonli.projectsnow.lhm=0-5

# 绝区零
com.miHoYo.Nap{UnityMain*}=6-7
com.miHoYo.Nap{UnityGfx*}=6-7
com.miHoYo.Nap=0-5

# 使命召唤:战争地带
com.activision.callofduty.warzone{UnityMain*}=6-7
com.activision.callofduty.warzone{UnityGfx*}=6-7
com.activision.callofduty.warzone{GameThread*}=6-7
com.activision.callofduty.warzone=0-5

# YGOPro3
com.YGO.MDPro3{UnityMain*}=6-7
com.YGO.MDPro3{UnityGfx*}=6-7
com.YGO.MDPro3=0-5

# 和平精英日韩服
com.pubg.krmobile{Thread-*}=6-7
com.pubg.krmobile{RenderThread*}=6-7
com.pubg.krmobile=0-5

# 和平精英国际服
com.tencent.ig{Thread-*}=6-7
com.tencent.ig{RenderThread*}=6-7
com.tencent.ig=0-5

# 鸣潮
com.kurogame.mingchao{Thread-*}=6-7
com.kurogame.mingchao{RenderThread*}=6-7
com.kurogame.mingchao{GameThread*}=6-7
com.kurogame.mingchao=0-5

# 晶核
com.hermes.p6game{Thread-*}=6-7
com.hermes.p6game{RenderThread*}=6-7
com.hermes.p6game{GameThread*}=6-7
com.hermes.p6game=0-5

# 晶核
com.hermes.p6game.aligames{Thread-*}=6-7
com.hermes.p6game.aligames{RenderThread*}=6-7
com.hermes.p6game.aligames{GameThread*}=6-7
com.hermes.p6game.aligames=0-5

# 晶核
com.hermes.p6game.huawei{Thread-*}=6-7
com.hermes.p6game.huawei{RenderThread*}=6-7
com.hermes.p6game.huawei{GameThread*}=6-7
com.hermes.p6game.huawei=0-5

# 暗区突围
com.tencent.mf.uam{Thread-*}=6-7
com.tencent.mf.uam{RenderThread*}=6-7
com.tencent.mf.uam{GameThread*}=6-7
com.tencent.mf.uam=0-5

# 巅峰极速
com.netease.race{Thread-*}=6-7
com.netease.race{RenderThread*}=6-7
com.netease.race{GameThread*}=6-7
com.netease.race=0-5

# 荒野行动
com.netease.hyxd.nearme.gamecenter{Thread-*}=6-7
com.netease.hyxd.nearme.gamecenter{RenderThread*}=6-7
com.netease.hyxd.nearme.gamecenter{GameThread*}=6-7
com.netease.hyxd.nearme.gamecenter=0-5

# 荒野行动
com.netease.hyxd.aligames{Thread-*}=6-7
com.netease.hyxd.aligames{RenderThread*}=6-7
com.netease.hyxd.aligames{GameThread*}=6-7
com.netease.hyxd.aligames=0-5

# 三角洲行动
com.tencent.tmgp.dfm{Thread-*}=6-7
com.tencent.tmgp.dfm{RenderThread*}=6-7
com.tencent.tmgp.dfm{GameThread*}=6-7
com.tencent.tmgp.dfm=0-5

# 极品飞车:集结
com.tencent.nfsonline{UnityMain*}=6-7
com.tencent.nfsonline{UnityGfx*}=6-7
com.tencent.nfsonline{GameThread*}=6-7
com.tencent.nfsonline=0-5

# 燕云十六声
com.netease.yyslscn{Thread-*}=6-7
com.netease.yyslscn{RenderThread*}=6-7
com.netease.yyslscn{GameThread*}=6-7
com.netease.yyslscn=0-5

# Pro象棋
vip.wqby.pro{RenderThread*}=6-7
vip.wqby.pro{binder*}=6-7
vip.wqby.pro=0-5

# JJ象棋
cn.jj.chess{UnityMain*}=6-7
cn.jj.chess{UnityGfx*}=6-7
cn.jj.chess=0-5

# 棋路
cn.apppk.apps.chessroad{RenderThread*}=6-7
cn.apppk.apps.chessroad=0-5

# 一念逍遥
com.leiting.xian{UnityMain*}=6-7
com.leiting.xian{UnityGfx*}=6-7
com.leiting.xian=0-5

# 狼人杀(网易)
com.netease.lrs{UnityMain*}=6-7
com.netease.lrs{UnityGfx*}=6-7
com.netease.lrs=0-5

# 无畏契约
com.tencent.tmgp.codev{GameThread}=6-7
com.tencent.tmgp.codev{RenderThread}=6-7
com.tencent.tmgp.codev=0-5">"$DIR/applist.conf"

echo "* 6+2线程配置执行完成"