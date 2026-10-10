#!/usr/bin/env bash
# ============================================================================
# MQTT 测试一键脚本
#
# 三层验证：
#   1) 协议单元层：测试套件内置 FakeBroker（进程内），验协议编解码细节
#   2) 端到端层  ：外部拉起真实 broker 子进程 + 真实客户端，验真实 socket 路径
#   3) 手工联调层：--manual 时保留 broker 运行，便于用外部工具观察真实报文
#
# 用法：
#   bash tools/mqtt_test.sh# 跑第 1、2 层
#   bash tools/mqtt_test.sh --manual   # 额外保留 broker 60 秒，供手工联调
#   bash tools/mqtt_test.sh --unit     # 只跑协议单元层（最快）
#   bash tools/mqtt_test.sh --e2e      # 只跑端到端层
#
# 说明：broker 由本脚本用 python 拉起，测试程序只做客户端。
#   这是刻意分离——broker 本就是独立外部服务；且部分受限环境下
#   QProcess 启动子进程会被系统拒绝（与代码无关）。
# ============================================================================
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SUITE="${QTDIR:-D:/programming/Qt/6.6.3/mingw_64}"
MINGW="${MINGW:-D:/programming/Qt/Tools/mingw1120_64}"

if [[ "$(uname -s)" =~ MINGW|MSYS|CYGWIN ]]; then
  to_unix(){ echo "$1" | sed -E 's|^([A-Za-z]):/|/\L\1/|'; }
  SUITE="$(to_unix "$SUITE")"; MINGW="$(to_unix "$MINGW")"
fi
export PATH="$SUITE/bin:$MINGW/bin:$PATH"

RED='\033[0;31m'; GRN='\033[0;32m'; CYA='\033[0;36m'; YEL='\033[0;33m'; RST='\033[0m'
FAIL=0
BROKER_PID=""
hr(){ printf "%s\n" "----------------------------------------------------------------------"; }

cleanup() {
  if [[ -n "$BROKER_PID" ]] && kill -0 "$BROKER_PID" 2>/dev/null; then
    kill "$BROKER_PID" 2>/dev/null
    wait "$BROKER_PID" 2>/dev/null
  fi
}
trap cleanup EXIT

MODE="${1:-basic}"

# ---------- 第1 层：协议单元测试 ----------
run_unit() {
  hr; printf "${CYA}[1/2] MQTT 协议单元测试（FakeBroker，进程内）${RST}\n"; hr
  local build="$ROOT/build/mqtt_unit"
  mkdir -p "$build"
  [[ -f "$ROOT/build/app6/.qmake.stash" ]] && cp "$ROOT/build/app6/.qmake.stash" "$build/" 2>/dev/null
  ( cd "$build" && qmake.exe "$ROOT/tests/tests.pro" "CONFIG+=release" >q.log 2>&1 \
    && mingw32-make.exe -j8 >b.log 2>&1 )
  if [[ $? -ne 0 ]]; then
    printf"  ${RED}[FAIL]${RST} 测试套件编译失败 → %s/b.log\n" "$build"
    grep -iE "error:" "$build/b.log" | head -5 | sed 's/^/         /'
    FAIL=1; return
  fi
  local out
  out=$(cd "$build/release" && ./fieldlink_tests.exe 2>&1)
  # 只显示 MQTT 段与总计，避免刷屏
  echo "$out" | sed -n '/=== MqttClient/,/=== CredentialCodec/p' \
    | grep -E "^\s+\[PASS\]|===" | sed 's/^/  /'
  local summary
  summary=$(echo "$out" | grep "PASS: ")
  printf"  ${GRN}全部用例：%s${RST}\n" "$summary"
  echo "$summary" | grep -q "FAIL: 0" || FAIL=1
}

# ---------- 第 2 层：端到端（外部起 broker） ----------
run_e2e() {
  hr; printf "${CYA}[2/2] MQTT 端到端测试（真实 broker 子进程）${RST}\n"; hr

  command -v python >/dev/null 2>&1 || {
    printf "  ${RED}[FAIL]${RST} 未找到 python（broker 脚本需要）"; FAIL=1; return; }

  local build="$ROOT/build/mqtt_e2e"
  mkdir -p "$build"
  [[ -f "$ROOT/build/app6/.qmake.stash" ]] && cp "$ROOT/build/app6/.qmake.stash" "$build/" 2>/dev/null

  # qmake 在 Windows 上需要 Windows 格式路径（D:/...），而 $ROOT 是 /c/...
  local wroot="${ROOT/\/c\//C:/}"
  cat > "$build/mqtt_e2e.pro" << EOF
QT += core network serialport
QT -= gui
CONFIG += c++11 console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = mqtt_e2e
INCLUDEPATH += $wroot/header
SOURCES += $wroot/tools/mqtt_e2e.cpp \\
    $wroot/src/mqttclient.cpp
HEADERS += $wroot/header/mqttclient.h $wroot/header/credentialcodec.h
EOF
  ( cd "$build" && qmake.exe mqtt_e2e.pro "CONFIG+=release" >q.log 2>&1 \
    && mingw32-make.exe -j8 >b.log 2>&1 )
  if [[ $? -ne 0 ]]; then
    printf "  ${RED}[FAIL]${RST} e2e 程序编译失败\n"
    grep -iE "error:|No rule to make|Failure to find" "$build/b.log" "$build/q.log" 2>/dev/null \
      | head -6 | sed 's/^/         /'
    FAIL=1; return
  fi
  local exe="$build/release/mqtt_e2e.exe"
  [[ -x "$exe" ]] || { printf "  ${RED}[FAIL]${RST} 未生成可执行文件\n"; FAIL=1; return; }

  # 起 broker（无认证 + 带认证各一份）
  local port=18885 authPort=18891
  python "$ROOT/slave/mqtt_test_broker.py" --port $port > "$build/broker.log" 2>&1 &
  BROKER_PID=$!
  python "$ROOT/slave/mqtt_test_broker.py" --port $authPort \
        --user admin --pass secret123 > "$build/broker_auth.log" 2>&1 &
  local authPid=$!

  # 等端口就绪
  local ok=0
  for _ in $(seq 1 40); do
    if (exec 3<>/dev/tcp/127.0.0.1/$port) 2>/dev/null; then exec 3<&- 3>&-; ok=1; break; fi
    sleep 0.25
  done
  if [[ $ok -ne 1 ]]; then
    printf "  ${RED}[FAIL]${RST} broker 未能在预期时间内监听 %d\n" "$port"
    sed 's/^/         /' "$build/broker.log" | head -5
    kill $authPid 2>/dev/null; FAIL=1; return
  fi
  printf "  broker 已就绪: 127.0.0.1:%d（无认证） / %d（admin/secret123）\n" "$port" "$authPort"

  # 跑客户端侧测试
  ( cd "$ROOT" && "$exe" --port $port 2>&1 ) | sed 's/^/  /'
  local rc=${PIPESTATUS[0]}
  [[ $rc -eq 0 ]] || FAIL=1

  # 认证用例
  ( cd "$ROOT" && "$exe" --port $authPort --user admin --pass secret123 2>&1 ) \
    | sed -n '/E2E-3/,/E2E-4/p' | sed 's/^/  /'

  kill $authPid 2>/dev/null; wait $authPid 2>/dev/null

  # 展示 broker 侧看到的报文（验证消息真的到达了服务端）
  if [[ -s "$build/broker.log" ]]; then
    printf "\n  ${YEL}broker 侧收到的 PUBLISH（前 12 条）:${RST}\n"
    grep -a "PUBLISH" "$build/broker.log" | cut -c1-140 | head -12 | sed 's/^/    /'
    local n
    n=$(grep -ac "PUBLISH" "$build/broker.log")
    printf "  broker 共收到 %s 条 PUBLISH\n" "$n"
    # 期望条数：E2E-1 发 3 条(qos0/retain/json) + E2E-2 发 1 条(qos1)
    #        + E2E-6 发 4 条(empty/big/中文主题/binary) = 8 条
    if [[ "$n" -lt 8 ]]; then
      printf "  ${RED}[FAIL]${RST} broker 只收到 %s/8 条，有消息未送达\n" "$n"
      FAIL=1
    else
      printf "  ${GRN}[OK]${RST} 8 条消息全部送达 broker\n"
    fi
  fi

  if [[ "$MODE" == "--manual" ]]; then
    printf "\n  ${YEL}[--manual]${RST} broker 保留 60 秒供手工联调…\n"
    printf "  另开终端订阅：python -c \"import socket;s=socket.create_connection(('127.0.0.1',$port));s.recv(99)\"\n"
    printf "  或直接观察 broker 日志：tail -f %s\n" "$build/broker.log"
    sleep 60
  fi
}

case "$MODE" in
  --unit) run_unit ;;
  --e2e)  run_e2e ;;
  *)
    run_unit
    run_e2e
    ;;
esac

hr
if [[ $FAIL -eq 0 ]]; then
  printf "${GRN}══ MQTT 测试全部通过 ══${RST}\n"
else
  printf "${RED}══ 存在失败项 ══${RST}\n"
fi

if [[ "$MODE" != "--unit" ]]; then
cat << 'GUIDE'

────────────────────────────────────────────────────────────────────────
手工联调：想亲眼看真实报文时
────────────────────────────────────────────────────────────────────────
1) 起 broker（会实时打印每条收到的 PUBLISH）：
     python slave/mqtt_test_broker.py --port 18883

2) 启动 FieldLink：Advanced → MQTT Publishing，填 127.0.0.1:18883 → 连接

3) broker 终端会逐条打印：
     PUBLISH topic=fieldlink/data/1/HoldingRegisters/0 retain=0 qos=0 bytes=123 payload={...}

4) 业务联动验证清单：
     · 配一条轮询任务（如读模拟从站 reg0），确认数据按
       fieldlink/data/{server}/{registerType}/{address} 上送
     · 触发报警，确认 alarm 主题有消息
     · 连接/断开设备，确认 status 主题有消息
     · 切QoS 0/1，确认 broker 侧 qos 字段随之变化
     · 杀掉 broker 再启动，确认自动重连且断连期间日志不刷屏

────────────────────────────────────────────────────────────────────────
分层说明
────────────────────────────────────────────────────────────────────────
· 协议单元层（tests/ 内FakeBroker）：验 CONNECT/PUBLISH/PINGREQ/PUBACK 编解码、
  retain 标志、QoS1 的 PUBACK 闭环、丢弃告警合并 —— 快、无外部依赖，适合频繁跑
· 端到端层（本脚本 + 真实 broker 子进程）：验真实 socket 路径、认证、真实载荷
  到达、用户意图门控、8KB 大载荷与 UTF-8 主题 —— 覆盖单元层测不到的部分
GUIDE
fi

exit $FAIL