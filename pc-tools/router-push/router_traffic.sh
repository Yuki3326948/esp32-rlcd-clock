#!/bin/sh
#
# router_traffic.sh —— 把路由器 WAN 口的实时网速与累计流量推给 ESP32 板子。
#
# 为什么不让板子自己来取:
#   这台 H68K 的 rpcd 把 root 的 ubus ACL 锁死了(file / luci 对象都没装,
#   network.interface dump 直接 Access denied),板子自己去登 LuCI 又得处理
#   表单登录 + cookie 过期。反过来让路由器主动推,路由器本来 7x24 开着,
#   比 PC 可靠,而且板子端只要一个几十行的 TCP 监听。
#
# 为什么用 3335 而不是 3334:
#   板子的 3334(sysmon)是单客户端设计 —— accept() 之后就死循环 read(),
#   而 PC 上的 temp_widget 是长连接常占着它。两条流必须分端口。
#
# ★ 为什么是"每秒一次性短连接"而不是挂着一条长连接 ★
#   实测踩到的坑:板子一复位,路由器这条长连接不会立刻报错 —— BusyBox 的
#   nc 不读 socket、只往里写,内核要等 TCP 重传耗尽(默认 tcp_retries2,
#   约 15 分钟)才认为对端没了。结果就是"板子重启后路由器十几分钟都不知道,
#   屏上的流量一直是旧值"。
#   改成每次发送都新建连接之后:板子不在时 connect 立刻返回 Connection
#   refused,下一秒就重试;板子一回来最多 2 秒就连上。
#   代价只有每秒一次 connect/close —— 对路由器和板子都微不足道。
#
#   ★ 但"连接是一次性的"并不等于"不会卡住":nc 写完数据后要等对端 FIN,
#     板子若正好在这一刻复位,FIN 不来,nc 就会永远停在 FIN_WAIT2,
#     把整个循环堵死。所以发送还必须【限时】—— 见下面 send_frame() 那段。
#     (2026-10-15 实际卡了 8 小时,温度和流量一起消失,就是栽在这里。)
#
# 行格式和 PC 端 temp_widget 完全一致(KEY=VALUE,空格分隔),
# 板子端复用同一套解析逻辑:
#   RTDN = 累计下行 (MiB)      RTUP = 累计上行 (MiB)
#   RTDS = 实时下行 (KiB/s)    RTUS = 实时上行 (KiB/s)
#
# 计数源 /sys/class/net/<iface>/statistics/{rx,tx}_bytes
#   = 自接口建立(pppoe 拨号)以来累计。重拨/重启会归零 —— 板子只显示它,
#   所以不影响;但**别把它当成"买回来至今"的终身总量**。
#
# 两个已验证的 BusyBox 前提(换机器/换固件要重新验):
#   1. ash 的算术是 64 位 —— rx_bytes 有 31 亿,32 位会翻身。
#      实测 3102465869/1048576 = 2958 正确。
#   2. printf 经管道是行刷新的,不会被块缓冲。
#
# ============================================================
#  天气(2026-10 加):室外温度 + WMO 天气码
# ============================================================
#  ★★ 为什么必须"先本地读 WAN IP,再按 IP 查位置"★★
#    这台路由器跑着 OpenClash,规则是【全局 TCP 重定向】:
#        -A openclash -p tcp -j REDIRECT --to-ports 7892
#    连路由器自己发出的请求也走代理出口。直接问 ip-api.com "我在哪",
#    它会按【代理出口】回答 —— 实测答的是"美国纽约州锡拉丘兹",
#    而真实 WAN 是云南电信。那样天气就完全错了。
#    改成"先读出自己的 WAN IP,再查这个 IP 在哪"就没事:
#    问的是一个【特定 IP】,谁问、走哪条路都不影响答案。实测拿到昆明 ✓
#
#  ★ 天气 10 分钟才变一次,查一次缓存起来,和每秒的流量一起发。
#    取不到就【不发这两个键】—— 板子是"收到过才置标志",不会显示成 0°C。
#
#  新增字段:
#    OTEMP = 室外温度(整数 °C,已四舍五入)
#    OWMO  = WMO 天气码(0=晴 / 1,2=少云多云 / 3=阴 / 45,48=雾
#                        61,63,65,80,81,82=雨 / 71~77,85,86=雪 / 95,96,99=雷)
#
#  数据源(都免注册免 key):
#    ip-api.com        —— 按 IP 反查经纬度
#    api.open-meteo.com —— 免 key 天气,直接给 WMO 码

BOARD_IP="${BOARD_IP:-192.168.1.182}"
BOARD_PORT="${BOARD_PORT:-3335}"
IFACE="${IFACE:-pppoe-wan}"
INTERVAL="${INTERVAL:-1}"

RXF="/sys/class/net/$IFACE/statistics/rx_bytes"
TXF="/sys/class/net/$IFACE/statistics/tx_bytes"

echo "router_traffic: $IFACE -> $BOARD_IP:$BOARD_PORT 每 ${INTERVAL}s(一次性短连接)"

prv_rx=0
prv_tx=0
prv_ts=0
was_up=0
fail_since=0

# ---------------- 天气 ----------------
WX_INTERVAL="${WX_INTERVAL:-600}"       # 天气刷新间隔(秒)
WX_TEMP=""                              # 缓存:室外温度(整数 °C)
WX_CODE=""                              # 缓存:WMO 天气码
wx_ts=0

# 取一次天气。成功返回 0 并更新 WX_TEMP / WX_CODE。
fetch_weather() {
wan=$(ip -4 addr show "$IFACE" 2>/dev/null | grep -o 'inet [0-9.]*' | cut -d' ' -f2)
[ -n "$wan" ] || return 1

geo=$(curl -s --max-time 8 "http://ip-api.com/json/$wan?fields=status,lat,lon" 2>/dev/null)
lat=$(echo "$geo" | sed -n 's/.*"lat":\([-0-9.]*\).*/\1/p')
lon=$(echo "$geo" | sed -n 's/.*"lon":\([-0-9.]*\).*/\1/p')
[ -n "$lat" ] && [ -n "$lon" ] || return 1

wx=$(curl -s --max-time 12 "https://api.open-meteo.com/v1/forecast?latitude=$lat&longitude=$lon&current=temperature_2m,weather_code&timezone=auto" 2>/dev/null)

# ★ open-meteo 的 JSON 里 temperature_2m / weather_code 各出现两次
#   (current_units 一次、current 一次)。sed 的 .* 是贪婪的,取到的是
#   【最后一次】= current 里的真值,正好。天气码那个模式要求以数字
#   开头,免得匹配到单位串 "wmo code"。
t=$(echo "$wx" | sed -n 's/.*"temperature_2m":\([-0-9.]*\).*/\1/p')
c=$(echo "$wx" | sed -n 's/.*"weather_code":\([0-9][0-9]*\).*/\1/p')
[ -n "$t" ] && [ -n "$c" ] || return 1

# 四舍五入到整数再发。板子那边 atoi 是【截断】不四舍五入,
# 在这里算干净,免得 22.7 显示成 22。
WX_TEMP=$(awk -v v="$t" 'BEGIN{printf "%d", (v < 0) ? v - 0.5 : v + 0.5}')
WX_CODE=$c
return 0
}

# ---------------- 发送(★ 必须限时,否则会永久卡死) ----------------
#  这台 BusyBox 的 nc 是精简版(用法就一行 `nc [IPADDR PORT]`):没有 -w,
#  系统里也没有 timeout 命令可用。而它把数据写完、shutdown 之后会一直
#  等对端回 FIN —— 板子只要在那一刻复位(掉电 / 看门狗重启 / 手动烧录),
#  这个 FIN 就永远不会来,nc 就永远停在 FIN_WAIT2 不动了。
#
#  ★ 实测踩到:脚本从 07:11 一直卡到 15:28(整整 8 小时),期间连天气都
#    不再刷新,屏上"室外温度"和"路由器上下行"一起消失。tcp_fin_timeout
#    (30s)救不了 —— 它只回收"没有进程持有 fd"的 FIN_WAIT2,而 nc 正攥着 fd。
#    上面"不可能卡住"那句注释是【错的】:连接确实是一次性的,但单次 nc
#    自己会无限等 FIN。这是整个服务唯一的单点,必须自己起看门狗兜住。
#
#  看门狗把 nc 收掉后,这一帧算失败,下一秒自然重连 —— 卡住最多损失
#  NC_TIMEOUT 秒,不会再把整个推送服务拖死。
NC_TIMEOUT=3

send_frame() {
	frame="/tmp/rt_frame.$$"
	printf 'RTDN=%u RTUP=%u RTDS=%u RTUS=%u%s\n' \
		"$1" "$2" "$3" "$4" "$5" > "$frame"

	# 用重定向而不是管道喂 nc:$! 就一定是 nc 自己的 pid,
	# 看门狗不会误杀同一条管道里的 printf。
	nc "$BOARD_IP" "$BOARD_PORT" < "$frame" 2>/dev/null &
	nc_pid=$!
	rm -f "$frame"

	( sleep "$NC_TIMEOUT"; kill -9 "$nc_pid" 2>/dev/null ) &
	wd_pid=$!

	wait "$nc_pid" 2>/dev/null
	rc=$?
	kill "$wd_pid" 2>/dev/null
	wait "$wd_pid" 2>/dev/null

	[ "$rc" = "0" ]
}

# 开机先取一次,免得板子要等满一个刷新周期才看到温度
if fetch_weather; then
wx_ts=$(date +%s)
echo "router_traffic: 天气 ${WX_TEMP}C WMO=$WX_CODE"
else
echo "router_traffic: 首次取天气失败,进循环后重试"
fi

while :; do
	sleep "$INTERVAL"

	# 拨号中/掉线时这个文件会消失,先不发
	[ -r "$RXF" ] || continue

	rx=$(cat "$RXF" 2>/dev/null) || continue
	tx=$(cat "$TXF" 2>/dev/null) || continue
	ts=$(date +%s)

	if [ "$prv_ts" -eq 0 ]; then
		# 第一帧没有基准,只记下来
		prv_rx=$rx
		prv_tx=$tx
		prv_ts=$ts
		continue
	fi

	dt=$((ts - prv_ts))
	[ "$dt" -lt 1 ] && dt=1

	drx=$((rx - prv_rx))
	dtx=$((tx - prv_tx))

	# 重拨 / 计数器被重置会产生负数。这一帧速度记 0,总量照发。
	[ "$drx" -lt 0 ] && drx=0
	[ "$dtx" -lt 0 ] && dtx=0

	# 总量走 MiB:字节数 32 位放不下(5TB 就溢出),MiB 能到 4PB
	dn=$((rx / 1048576))
	up=$((tx / 1048576))
	ds=$((drx / dt / 1024))
	us=$((dtx / dt / 1024))

	prv_rx=$rx
	prv_tx=$tx
	prv_ts=$ts

	# 天气到期就重取。失败 60 秒后重试(而不是等满 10 分钟),
	# 这样路由器刚重启、刚拨上号时能很快补上。
	if [ $((ts - wx_ts)) -ge "$WX_INTERVAL" ]; then
		if fetch_weather; then
			wx_ts=$ts
			echo "router_traffic: 天气 ${WX_TEMP}C WMO=$WX_CODE"
		else
			wx_ts=$((ts - WX_INTERVAL + 60))
		fi
	fi

	# 天气字段跟着流量一起发。没取到就一个都不发 —— 板子是
	# "收到过才置标志",不会把 0 当成 0°C。
	WXF=""
	[ -n "$WX_TEMP" ] && WXF=" OWMO=$WX_CODE OTEMP=$WX_TEMP"
	# ★ 顺序要紧:OWMO 必须在 OTEMP 【之前】。板子是在解析到 OTEMP
	#   那一刻记日志的,天气码先到,日志里打出来的才是配对的这一组,
	#   否则会显示成上一轮的天气码(差 1 秒,但排查时很误导)。

	# ★ 每次都是新连接。板子不在 -> 立刻 Connection refused,不阻塞。
	#   发送本身由 send_frame 限时(见上面那段),卡住也不会拖死脚本。
	if send_frame "$dn" "$up" "$ds" "$us" "$WXF"; then
		if [ "$was_up" = "0" ]; then
			echo "router_traffic: 已连上板子 $BOARD_IP:$BOARD_PORT"
			was_up=1
		fi
		fail_since=0
	else
		if [ "$was_up" = "1" ]; then
			echo "router_traffic: 与板子断开,持续重试"
			was_up=0
			fail_since=$ts
		elif [ "$fail_since" = "0" ]; then
			fail_since=$ts
			echo "router_traffic: 连不上 $BOARD_IP:$BOARD_PORT,每秒重试中"
		fi
	fi
done
