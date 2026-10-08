#!/system/bin/sh
# =====================================================================
# 慕容调度 · FAS 频率生成器 (bin/cpu/gen_fas.sh)
#
# 作用：按【本机真实 CPU 档位表】生成 bin/cpu/fas.json 里的
#       sweetspot_freq（甜点频率）与 deep_floor_freq（深睡地板），
#       其它字段（defaults / apps / projects / _murong_remote）原样保留。
#
# 调用时机：安装期（customize.sh）。绝不能每次开机重写：
#   用户会在软件里手改 fas.json，重写等于冲掉用户配置。
#
# 【已存在则不覆盖】判定顺序：
#   1. 目标 bin/cpu/fas.json 已存在           -> 直接退出
#   2. 上一次安装的模块目录里已有 fas.json     -> 原样继承（用户可能手改过）
#      仅当新旧模块 module.prop 的 name 一致时才继承，
#      避免从「慕容调度」切到「慕容线程」时把 FAS 配置带过去
#   3. 都不成立才生成（模板 fas.default.json + 本机档位表）
#
# 生成规则（结果一定落在本机档位内）：
#   甜点 = 模式走廊 min + (max-min) * SS_PM/1000，吸附到该簇档位表最近档
#   地板 = 甜点 * DF_PM/1000，吸附到 [该簇最低档, 甜点] 内最近档
#   模式走廊取 bin/cpu/<SoC>.json 里每个模式每个 cluster 的 min_freq/max_freq。
#   SS_PM / DF_PM 由 SM8850 上现有 fas.json 反推（见下方常量），
#   所以在 SM8850 上生成结果与现有配置逐值一致，换 SoC 时按同比例缩放。
#   簇 0 用第 1 个比例；簇 >= 1 用第 2 个比例（2 簇机型「大核」口径，
#   3/4 簇机型的 mid/prime 沿用同一口径）；只有 1 个比例时所有簇共用。
#
# 档位表来源（并集，两者都算合法值）：
#   /sys/devices/system/cpu/cpufreq/policyN/scaling_available_frequencies
#   /sys/devices/system/cpu/cpufreq/policyN/scaling_boost_frequencies
#   例：SM8850 大核 4512000/4608000 只存在于 boost 表。
#
# 兜底：读不到档位表 / 找不到 SoC 配置 / awk 失败
#   -> 频率块保持空对象，绝不写入其它 SoC 的硬编码频率。
#
# 测试钩子（仅本地验证用）：MURONG_SYSFS_ROOT / MURONG_CPU_MODEL
# =====================================================================

set -u

MODPATH_ARG="${1:-}"
FORCE=0
[ "${2:-}" = "--force" ] && FORCE=1

if [ -z "$MODPATH_ARG" ]; then
    echo "gen_fas: 缺少模块目录参数" >&2
    exit 2
fi

CPU_DIR="$MODPATH_ARG/bin/cpu"
TARGET="$CPU_DIR/fas.json"
TEMPLATE="$CPU_DIR/fas.default.json"
SYSFS_ROOT="${MURONG_SYSFS_ROOT:-/sys}"
MODULE_ID="muronggameopt"
# 测试钩子：MURONG_OLD_MODULE_DIR 可指向假的上一次安装目录
OLD_MODULE_DIR="${MURONG_OLD_MODULE_DIR:-/data/adb/modules/$MODULE_ID}"

# ---- 比例（千分比，由 SM8850 现有 fas.json 反推）------------------------
# 甜点比例：模式:簇0/簇>=1
SS_PM="powersave:354/234 balance:297/107 performance:399/189 fast:542/360"
# 深睡地板比例（相对甜点）：模式:簇0[/簇>=1]，"-" 表示该簇不显式写（内核 65% 兜底）
DF_PM="powersave:680 balance:592/736 performance:655/682 fast:630/587"

log() { echo "gen_fas: $*"; }

# 1) 目标已存在 -> 不覆盖
if [ -f "$TARGET" ] && [ "$FORCE" != "1" ]; then
    log "fas.json 已存在，跳过生成（保留现有配置）"
    exit 0
fi

# 2) 上一次安装留下的 fas.json（用户可能手改过）优先继承
if [ "$FORCE" != "1" ] && [ -f "$OLD_MODULE_DIR/bin/cpu/fas.json" ]; then
    # 变体名比较前去掉结尾版本号：免费版 module.prop 的 name 会写成「慕容调度6.0」，
    # 不归一化的话每次更新都会被误判成换变体，用户手改的 fas.json 就继承不到了
    norm_name() { printf '%s' "$1" | sed 's/[0-9][0-9.]*$//'; }
    old_name="$(norm_name "$(sed -n 's/^name=//p' "$OLD_MODULE_DIR/module.prop" 2>/dev/null | head -n 1 | tr -d '\r')")"
    new_name="$(norm_name "$(sed -n 's/^name=//p' "$MODPATH_ARG/module.prop" 2>/dev/null | head -n 1 | tr -d '\r')")"
    if [ -n "$old_name" ] && [ "$old_name" = "$new_name" ]; then
        if cp -f "$OLD_MODULE_DIR/bin/cpu/fas.json" "$TARGET" 2>/dev/null; then
            log "沿用已有 fas.json（未覆盖用户配置）"
            exit 0
        fi
    else
        log "检测到模块变体切换（$old_name -> $new_name），不继承旧 fas.json"
    fi
fi

if [ ! -f "$TEMPLATE" ]; then
    log "缺少模板 $TEMPLATE，放弃生成"
    exit 1
fi

# ---- 本机策略（簇序号 = policy 升序，与 daemon detect_cpu_clusters 一致）----
# 真实目录（cpu*/cpufreq/policyN），policy 号升序 = daemon 的簇序号；
# 兜底再试 /sys/devices/system/cpu/cpufreq/policyN。
POLICY_LIST=""
for d in "$SYSFS_ROOT"/devices/system/cpu/cpu*/cpufreq/policy*; do
    [ -d "$d" ] || continue
    n="${d##*policy}"
    case "$n" in
        ''|*[!0-9]*) continue ;;
    esac
    POLICY_LIST="$POLICY_LIST$n $d
"
done
if [ -z "$POLICY_LIST" ]; then
    for d in "$SYSFS_ROOT"/devices/system/cpu/cpufreq/policy*; do
        [ -d "$d" ] || continue
        n="${d##*policy}"
        case "$n" in
            ''|*[!0-9]*) continue ;;
        esac
        POLICY_LIST="$POLICY_LIST$n $d
"
    done
fi
POLICY_LIST="$(printf '%s\n' "$POLICY_LIST" | grep -E '^[0-9]+ ' | sort -n | awk '!seen[$1]++')"

collect_steps() {
    avail="$(cat "$1/scaling_available_frequencies" 2>/dev/null)"
    boost="$(cat "$1/scaling_boost_frequencies" 2>/dev/null)"
    echo "$avail $boost" | tr ' ' '\n' | grep -E '^[0-9]+$' | sort -n -u | tr '\n' ' '
}

# ---- SoC 型号与模式走廊 ----
detect_model() {
    if [ -n "${MURONG_CPU_MODEL:-}" ]; then
        echo "${MURONG_CPU_MODEL}"
        return
    fi
    raw="$(getprop ro.soc.model 2>/dev/null)"
    [ -n "$raw" ] || raw="$(getprop ro.board.platform 2>/dev/null)"
    [ -n "$raw" ] || raw="$(getprop ro.hardware 2>/dev/null)"
    upper="$(echo "$raw" | tr 'a-z' 'A-Z')"
    model="$(echo "$upper" | sed -n 's/^.*\(SM[0-9][0-9A-Z_-]*\).*$/\1/p' | head -n 1)"
    [ -n "$model" ] || model="$(echo "$upper" | sed -n 's/^.*\(MT[0-9][0-9][0-9][0-9]\).*$/\1/p' | head -n 1)"
    echo "$model"
}

SOC_MODEL="$(detect_model)"
SOC_JSON=""
if [ -n "$SOC_MODEL" ]; then
    if [ -f "$CPU_DIR/$SOC_MODEL.json" ]; then
        SOC_JSON="$CPU_DIR/$SOC_MODEL.json"
    else
        for f in "$CPU_DIR/$SOC_MODEL"*.json; do
            [ -f "$f" ] || continue
            SOC_JSON="$f"
            break
        done
    fi
fi
[ -n "$SOC_JSON" ] && log "SoC: $SOC_MODEL -> ${SOC_JSON##*/}"

CORRIDORS=""
if [ -n "$SOC_JSON" ]; then
    CORRIDORS="$(awk '
        function flush(   c) {
            if (mode != "") {
                for (c = 0; c < 8; c++) {
                    if (mn[c] > 0 && mx[c] > 0) print mode, c, mn[c], mx[c]
                }
            }
            for (c = 0; c < 8; c++) { mn[c] = 0; mx[c] = 0 }
            cur = -1
        }
        /^[ \t]*"[A-Za-z_]+"[ \t]*:[ \t]*\{[ \t]*$/ {
            ind = 0
            while (substr($0, ind + 1, 1) == " ") ind++
            key = $0
            sub(/^[ \t]*"/, "", key)
            sub(/".*$/, "", key)
            if (mode == "" || ind <= mode_ind) {
                if (key == "powersave" || key == "balance" || key == "performance" || key == "fast") {
                    flush()
                    mode = key
                    mode_ind = ind
                } else if (ind <= mode_ind) {
                    flush()
                    mode = ""
                    mode_ind = -1
                }
            }
            next
        }
        mode != "" && /^[ \t]*"cluster"[ \t]*:/ { v = $0; gsub(/[^0-9]/, "", v); cur = v + 0; next }
        mode != "" && cur >= 0 && /^[ \t]*"min_freq"[ \t]*:/ { v = $0; gsub(/[^0-9]/, "", v); if (v + 0 > 0) mn[cur] = v + 0; next }
        mode != "" && cur >= 0 && /^[ \t]*"max_freq"[ \t]*:/ { v = $0; gsub(/[^0-9]/, "", v); if (v + 0 > 0) mx[cur] = v + 0; next }
        END { flush() }
    ' "$SOC_JSON")"
fi

TABLE=""
if [ -n "$CORRIDORS" ] && [ -n "$POLICY_LIST" ]; then
    idx=0
    while read -r p pdir; do
        [ -n "$pdir" ] || continue
        steps="$(collect_steps "$pdir")"
        if [ -n "$steps" ]; then
            rows="$(echo "$CORRIDORS" | awk -v c="$idx" -v s="$steps" '$2 == c { print $1, $2, $3, $4, s }')"
            [ -n "$rows" ] && TABLE="$TABLE$rows
"
        fi
        idx=$((idx + 1))
    done <<EOF
$POLICY_LIST
EOF
fi

if [ -z "$TABLE" ]; then
    log "读不到本机档位表或模式走廊，频率块留空（不写入任何硬编码频率）"
fi

# ---- 生成：把模板里的哨兵行替换成按本机档位算出来的频率块 ----
OUT="$TARGET.new"
rm -f "$OUT"

awk -v table="$TABLE" -v ss_pm="$SS_PM" -v df_pm="$DF_PM" -v tmpl="$TEMPLATE" '
    function snap(target, lo, hi, k,   i, v, d, best, bd) {
        best = -1; bd = -1
        for (i = 1; i <= sn[k]; i++) {
            v = ST[k, i]
            if (v < lo || v > hi) continue
            d = (v > target) ? v - target : target - v
            if (bd < 0 || d < bd || (d == bd && v < best)) { bd = d; best = v }
        }
        if (best < 0) {
            for (i = 1; i <= sn[k]; i++) {
                v = ST[k, i]
                d = (v > target) ? v - target : target - v
                if (bd < 0 || d < bd) { bd = d; best = v }
            }
        }
        return best
    }
    function pm_of(spec, mode, c,   n, i, a, b, m, p, idx) {
        n = split(spec, a, " ")
        for (i = 1; i <= n; i++) {
            m = split(a[i], b, ":")
            if (m < 2 || b[1] != mode) continue
            idx = split(b[2], p, "/")
            if (idx <= 0) return ""
            if (c == 0) return p[1]
            return (idx >= 2) ? p[2] : ""
        }
        return ""
    }
    BEGIN {
        MODEORDER[1] = "powersave"; MODEORDER[2] = "balance"
        MODEORDER[3] = "performance"; MODEORDER[4] = "fast"

        nl = split(table, L, "\n")
        for (k = 1; k <= nl; k++) {
            if (L[k] == "") continue
            nf = split(L[k], f, " ")
            if (nf < 5) continue
            cnt++
            M[cnt] = f[1]; C[cnt] = f[2] + 0; MN[cnt] = f[3] + 0; MX[cnt] = f[4] + 0
            sn[cnt] = 0
            for (i = 5; i <= nf; i++) { sn[cnt]++; ST[cnt, sn[cnt]] = f[i] + 0 }
        }

        for (k = 1; k <= cnt; k++) {
            if (MX[k] <= MN[k]) continue
            p = pm_of(ss_pm, M[k], C[k])
            if (p == "") continue
            raw = MN[k] + int((MX[k] - MN[k]) * p / 1000)
            v = snap(raw, MN[k], MX[k], k)
            if (v > 0) SS[M[k], C[k]] = v
        }
        for (k = 1; k <= cnt; k++) {
            if (!((M[k], C[k]) in SS)) continue
            p = pm_of(df_pm, M[k], C[k])
            if (p == "") continue
            sweet = SS[M[k], C[k]]
            v = snap(int(sweet * p / 1000), ST[k, 1], sweet, k)
            if (v > 0) DF[M[k], C[k]] = v
        }

        nb = 0
        for (mi = 1; mi <= 4; mi++) {
            mode = MODEORDER[mi]
            n = 0
            for (k = 1; k <= cnt; k++) if (M[k] == mode && ((mode, C[k]) in SS)) { n++; cl[n] = C[k] }
            if (n == 0) continue
            for (i = 1; i <= n; i++) for (j = i + 1; j <= n; j++) if (cl[j] < cl[i]) { t = cl[i]; cl[i] = cl[j]; cl[j] = t }
            ssl[++nss] = "    \"" mode "\": {"
            for (i = 1; i <= n; i++) {
                sep = (i < n) ? "," : ""
                ssl[++nss] = "      \"" cl[i] "\": " SS[mode, cl[i]] sep
            }
            ssl[++nss] = "    }"
            nb++; endidx[nb] = nss
        }
        for (i = 1; i < nb; i++) ssl[endidx[i]] = ssl[endidx[i]] ","

        nb = 0
        for (mi = 1; mi <= 4; mi++) {
            mode = MODEORDER[mi]
            n = 0
            for (k = 1; k <= cnt; k++) if (M[k] == mode && ((mode, C[k]) in DF)) { n++; cl[n] = C[k] }
            if (n == 0) continue
            for (i = 1; i <= n; i++) for (j = i + 1; j <= n; j++) if (cl[j] < cl[i]) { t = cl[i]; cl[i] = cl[j]; cl[j] = t }
            dfl[++ndf] = "    \"" mode "\": {"
            for (i = 1; i <= n; i++) {
                sep = (i < n) ? "," : ""
                dfl[++ndf] = "      \"" cl[i] "\": " DF[mode, cl[i]] sep
            }
            dfl[++ndf] = "    }"
            nb++; endidx[nb] = ndf
        }
        for (i = 1; i < nb; i++) dfl[endidx[i]] = dfl[endidx[i]] ","

        while ((getline line < tmpl) > 0) {
            if (index(line, "__MURONG_GENERATED_SWEETSPOT__") > 0) {
                for (i = 1; i <= nss; i++) print ssl[i]
                continue
            }
            if (index(line, "__MURONG_GENERATED_DEEP_FLOOR__") > 0) {
                for (i = 1; i <= ndf; i++) print dfl[i]
                continue
            }
            print line
        }
        close(tmpl)
    }
' > "$OUT" 2>/dev/null

if [ -s "$OUT" ] && grep -q '"apps"' "$OUT" && grep -q '"defaults"' "$OUT"; then
    mv -f "$OUT" "$TARGET"
    log "已写入 fas.json（频率已按本机真实档位吸附；无档位表时留空）"
else
    rm -f "$OUT"
    if cp -f "$TEMPLATE" "$TARGET" 2>/dev/null; then
        log "生成失败，已回退为模板副本（未写入任何频率）"
    else
        log "生成失败，且模板复制也失败，保持无 fas.json"
        exit 1
    fi
fi

exit 0
