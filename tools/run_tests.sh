#!/usr/bin/env bash
# ============================================================================
# FieldLink 一键回归脚本
# ----------------------------------------------------------------------------
# 用法：
#   bash tools/run_tests.sh              # 全部层级
#   bash tools/run_tests.sh unit         # 只跑单元/集成测试（最快，~1 分钟）
#   bash tools/run_tests.sh slave        # 只跑模拟从站协议自检
#   bash tools/run_tests.sh build        # 只做主程序编译验证
#   bash tools/run_tests.sh clean        # 清理构建产物
#
# 环境变量（可选）：
#   QTDIR=D:/programming/Qt/6.6.3/mingw_64      指定 Qt 套件
#   MINGW=D:/programming/Qt/Tools/mingw1120_64  指定 MinGW 工具链
# ============================================================================
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"
SUITE="${QTDIR:-D:/programming/Qt/6.6.3/mingw_64}"
MINGW="${MINGW:-D:/programming/Qt/Tools/mingw1120_64}"

# Git-Bash on Windows:把 D:/a/b 转成 /d/a/b，否则 PATH 里的 exe 找不到
if [[ "$(uname -s)" =~ MINGW|MSYS|CYGWIN ]]; then
  to_unix(){ echo "$1" | sed -E 's|^([A-Za-z]):/|/\L\1/|'; }
  SUITE="$(to_unix "$SUITE")"; MINGW="$(to_unix "$MINGW")"
fi
export PATH="$SUITE/bin:$MINGW/bin:$PATH"

RED='\033[0;31m'; GRN='\033[0;32m'; YEL='\033[0;33m'; CYA='\033[0;36m'; RST='\033[0m'
PASS_CNT=0; FAIL_CNT=0
declare -a FAILED_ITEMS

hr(){ printf "%s\n" "----------------------------------------------------------------------"; }
ok(){   PASS_CNT=$((PASS_CNT+1)); printf "  ${GRN}[PASS]${RST} %s\n" "$1"; }
bad(){  FAIL_CNT=$((FAIL_CNT+1)); FAILED_ITEMS+=("$1"); printf "  ${RED}[FAIL]${RST} %s\n" "$1"; }

# ---------------------------------------------------------------------------
# 层级 1：编译验证（主程序 + 测试套件，0 error 才算过）
# ---------------------------------------------------------------------------
do_build() {
  hr; printf "${CYA}[1/4] 编译验证${RST}\n"; hr
  [[ -x "$SUITE/bin/qmake.exe" ]] || { bad "找不到 qmake.exe（$SUITE/bin）"; return 1; }
  printf "  Qt    : %s\n" "$SUITE"
  printf "  MinGW : %s\n\n" "$MINGW"

  # 主程序
  local d="$BUILD/verify_app"
  mkdir -p "$d"
  # 复用已存在的 .qmake.stash 可绕过 MinGW 工具链探测失败（见 memory 记录）
  [[ -f "$BUILD/app6/.qmake.stash" ]] && cp "$BUILD/app6/.qmake.stash" "$d/" 2>/dev/null
  ( cd "$d" && qmake.exe "$ROOT/fieldlink.pro" "CONFIG+=release" >qmake.log 2>&1 \
    && mingw32-make.exe -j8 >build.log 2>&1 )
  if [[ $? -eq 0 ]]; then
    ok "主程序编译通过（fieldlink.exe）"
  else
    bad "主程序编译失败 → $d/build.log"
    grep -i "error" "$d/build.log" 2>/dev/null | head -10 | sed 's/^/         /'
    return 1
  fi

  # 测试套件
  local t="$BUILD/verify_tests"
  mkdir -p "$t"
  [[ -f "$BUILD/app6/.qmake.stash" ]] && cp "$BUILD/app6/.qmake.stash" "$t/" 2>/dev/null
  ( cd "$t" && qmake.exe "$ROOT/tests/tests.pro" "CONFIG+=release" >qmake.log 2>&1 \
    && mingw32-make.exe -j8 >build.log 2>&1 )
  if [[ $? -eq 0 ]]; then
    ok "测试套件编译通过（fieldlink_tests.exe）"
  else
    bad "测试套件编译失败 → $t/build.log"
    grep -i "error" "$t/build.log" 2>/dev/null | head -10 | sed 's/^/         /'
    return 1
  fi
  printf "\n  ${YEL}测试二进制: %s${RST}\n" "$t/release/fieldlink_tests.exe"
}

# ---------------------------------------------------------------------------
# 层级 2：单元/集成测试（无 GUI，退出码 0 = 全通过）
# ---------------------------------------------------------------------------
do_unit() {
  hr; printf "${CYA}[2/4] 单元 / 集成测试（158 项断言）${RST}\n"; hr
  local exe="$BUILD/verify_tests/release/fieldlink_tests.exe"
  [[ -x "$exe" ]] || { bad "测试二进制不存在，请先运行: bash tools/run_tests.sh build"; return 1; }
  ( cd "$(dirname "$exe")" && ./fieldlink_tests.exe >unit.log 2>&1 )
  local rc=$?
  cat "$(dirname "$exe")/unit.log" | sed 's/^/  /'
  if [[ $rc -eq 0 ]]; then ok "退出码 0 → 全部通过"
  else bad "退出码 $rc → 有用例失败，详见上方 [FAIL] 行"; fi
}

# ---------------------------------------------------------------------------
# 层级 3：模拟从站协议自检（9 项，验证 slave/ 脚本本身没坏）
# ---------------------------------------------------------------------------
do_slave() {
  hr; printf "${CYA}[3/4] 模拟从站协议自检${RST}\n"; hr
  if ! command -v python >/dev/null 2>&1; then bad "未找到 python"; return 1; fi
  printf "  Python: %s\n\n" "$(python --version 2>&1)"
  python "$ROOT/slave/modbus_tcp_simulator.py" --selftest 2>&1 | sed 's/^/  /'
  local rc=${PIPESTATUS[0]}
  [[ $rc -eq 0 ]] && ok "从站自检通过（8 功能码 + 2 类异常码）" || bad "从站自检失败（rc=$rc）"
}

# ---------------------------------------------------------------------------
# 层级 4：环境体检
# ---------------------------------------------------------------------------
do_env() {
  hr; printf "${CYA}[4/4] 环境体检${RST}\n"; hr
  command -v python >/dev/null 2>&1 && ok "Python 3.x（内置从站 / MQTT broker 需要）" || bad "缺 Python"
  [[ -d "$SUITE" ]] && ok "Qt 套件存在：$SUITE" || bad "缺 Qt：$SUITE"
  [[ -d "$MINGW" ]] && ok "MinGW 存在：$MINGW" || bad "缺 MinGW：$MINGW"
  if git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1; then
    local dirty; dirty=$(git -C "$ROOT" status --porcelain | wc -l)
    if [[ "$dirty" -eq 0 ]]; then ok "Git 工作区干净"
    else printf "  ${YEL}[WARN]${RST} %s 个未提交改动（不影响测试）\n" "$dirty"; fi
  fi
  # 覆盖率盲区统计
  local total cov
  total=$(ls "$ROOT"/src/*.cpp 2>/dev/null | wc -l)
  cov=0
  for f in "$ROOT"/src/*.cpp; do
    grep -q "$(basename "$f")" "$ROOT/tests/tests.pro" 2>/dev/null && cov=$((cov+1))
  done
  printf "\n  自动化覆盖: %s/%s 个源文件进入测试套件\n" "$cov" "$total"
  local gap=$((total - cov))
  [[ $gap -gt 0 ]] && printf "  ${YEL}[INFO]${RST} %s 个模块无自动化测试 → 见 doc/TEST_PLAN.md 第三层「GUI 手工用例」\n" "$gap"
}

do_clean(){
  printf "清理 %s 下的 verify_* 构建目录...\n" "$BUILD"
  rm -rf "$BUILD/verify_app" "$BUILD/verify_tests"
  ok "已清理"
}

case "${1:-all}" in
  build) do_build ;;
  unit)  do_build && do_unit ;;
  slave) do_slave ;;
  env)   do_env ;;
  clean) do_clean ;;
  all)   do_build && do_unit && do_slave && do_env ;;
  *)     printf "用法: bash tools/run_tests.sh [all|build|unit|slave|env|clean]\n"; exit 2 ;;
esac

hr
if [[ $FAIL_CNT -eq 0 ]]; then
  printf "${GRN}══ 全部通过：%d 项 ══${RST}\n" "$PASS_CNT"
  exit 0
else
  printf "${RED}══ 失败 %d 项 / 通过 %d 项 ══${RST}\n" "$FAIL_CNT" "$PASS_CNT"
  for i in "${FAILED_ITEMS[@]}"; do printf "  ${RED}· %s${RST}\n" "$i"; done
  exit 1
fi
