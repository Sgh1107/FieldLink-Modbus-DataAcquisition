# mqtt_test.ps1 —— MQTT 测试一键脚本（Windows / PowerShell）
#
# 与 tools/mqtt_test.sh 等价，供没有 bash（Git Bash / WSL）的 Windows 环境使用。
#
# 三层验证：
#   1) 协议单元层：tests/ 内置 FakeBroker（进程内），验协议编解码细节
#   2) 端到端层  ：外部拉起 mini broker 子进程 + 真实客户端，验真实 socket 路径
#   3) 手工联调层：-Mode manual 时保留 broker 运行，便于观察真实报文
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File tools\mqtt_test.ps1
#   powershell -ExecutionPolicy Bypass -File tools\mqtt_test.ps1 -Mode unit
#   powershell -ExecutionPolicy Bypass -File tools\mqtt_test.ps1 -Mode e2e
#   powershell -ExecutionPolicy Bypass -File tools\mqtt_test.ps1 -Mode manual -Port 18883
#
# 参数：
#   -Mode      all（默认）| unit | e2e | manual
#   -Port      无认证 broker 端口（默认 18885）
#   -AuthPort  带认证 broker 端口（默认 18891，用户名 admin / 密码 secret123）
#   -QtDir     Qt 套件目录（默认 D:\programming\Qt\6.6.3\mingw_64）
#   -MinGwDir  MinGW 工具链目录（默认 D:\programming\Qt\Tools\mingw1120_64）
#
# 退出码：0 = 全部通过，1 = 有失败

[CmdletBinding()]
param(
    [ValidateSet('all', 'unit', 'e2e', 'manual')]
    [string]$Mode = 'all',
    [int]$Port = 18885,
    [int]$AuthPort = 18891,
    [string]$QtDir = 'D:\programming\Qt\6.6.3\mingw_64',
    [string]$MinGwDir = 'D:\programming\Qt\Tools\mingw1120_64',
    # 单元层默认跑全量（含 MQTT 连接超时/重连等依赖真实时间的用例）。
    # 加 -Fast 可跳过这些慢速用例，把单元层从 ~80s 压到 ~30s。
    [switch]$Fast
)

$ErrorActionPreference = 'Continue'
$env:PATH = "$MinGwDir\bin;$QtDir\bin;$env:PATH"
# 让 broker / 客户端脚本的 stdout 直接按 UTF-8 输出，避免 GBK 控制台乱码
$env:PYTHONIOENCODING = 'utf-8'
$env:PYTHONUTF8 = '1'

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build\mqtt_test'
$proRoot = $root.Replace('\', '/')

$script:Fail = 0
$script:Brokers = @()

function Write-Head([string]$text) {
    Write-Host ('-' * 70)
    Write-Host $text -ForegroundColor Cyan
    Write-Host ('-' * 70)
}

function Wait-Port([int]$p, [int]$timeoutSec = 15) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        try {
            $client = New-Object System.Net.Sockets.TcpClient
            $client.Connect('127.0.0.1', $p)
            $client.Close()
            return $true
        } catch {
            Start-Sleep -Milliseconds 200
        }
    }
    return $false
}

function Start-Broker([int]$p, [string[]]$extraArgs = @()) {
    $log = Join-Path $build ("broker_$p.log")
    $err = Join-Path $build ("broker_$p.err.log")
    Remove-Item $log, $err -Force -ErrorAction SilentlyContinue
    # -u：不缓冲，保证被杀掉时日志也完整
    $argv = @('-u', (Join-Path $root 'slave\mqtt_test_broker.py'), '--port', "$p") + $extraArgs
    $proc = Start-Process -FilePath 'python' -ArgumentList $argv -NoNewWindow -PassThru `
        -RedirectStandardOutput $log -RedirectStandardError $err
    $script:Brokers += $proc
    if (-not (Wait-Port $p)) {
        Write-Host "  [FAIL] broker 未能在预期时间内监听 $p" -ForegroundColor Red
        if (Test-Path $log) { Get-Content $log | Select-Object -First 10 | ForEach-Object { Write-Host "         $_" } }
        $script:Fail = 1
        return $null
    }
    Write-Host "  broker 已就绪: 127.0.0.1:$p"
    return $proc
}

function Stop-Brokers {
    foreach ($b in $script:Brokers) {
        if ($b -and -not $b.HasExited) { Stop-Process -Id $b.Id -Force -ErrorAction SilentlyContinue }
    }
    $script:Brokers = @()
}

function Build-E2E {
    New-Item -ItemType Directory -Force -Path $build | Out-Null
    $pro = Join-Path $build 'mqtt_e2e.pro'
    $content = @"
QT += core network serialport sql
QT -= gui
CONFIG += c++17 console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = mqtt_e2e
INCLUDEPATH += $proRoot/header
SOURCES += $proRoot/tools/mqtt_e2e.cpp \
    $proRoot/src/mqttclient.cpp \
    $proRoot/src/mqttoutbox.cpp
HEADERS += $proRoot/header/mqttclient.h $proRoot/header/mqttoutbox.h $proRoot/header/credentialcodec.h
"@
    # 注意：不能用 Set-Content -Encoding UTF8 —— PowerShell 5.1 会写入 BOM，
    # qmake 无法解析带 BOM 的 .pro（报 "Error processing project file"）。
    [System.IO.File]::WriteAllText($pro, $content, (New-Object System.Text.UTF8Encoding($false)))
    Push-Location $build
    & qmake.exe $pro 'CONFIG+=release' *> qmake.log
    if ($LASTEXITCODE -ne 0) {
        Write-Host '  [FAIL] e2e 工程 qmake 失败' -ForegroundColor Red
        Get-Content qmake.log | Select-String 'ERROR' | Select-Object -First 5 | ForEach-Object { Write-Host "         $_" }
        Pop-Location; $script:Fail = 1; return $null
    }
    & mingw32-make.exe -j8 *> make.log
    $rc = $LASTEXITCODE
    Pop-Location
    if ($rc -ne 0) {
        Write-Host '  [FAIL] e2e 程序编译失败' -ForegroundColor Red
        Get-Content (Join-Path $build 'make.log') | Select-String 'error:' | Select-Object -First 8 | ForEach-Object { Write-Host "         $_" }
        $script:Fail = 1; return $null
    }
    return (Join-Path $build 'release\mqtt_e2e.exe')
}

# ---------- 第 1 层：协议单元测试 ----------
function Invoke-Unit {
    Write-Head '[1/2] MQTT 协议单元测试（FakeBroker，进程内）'
    $unitBuild = Join-Path $root 'build\mqtt_unit'
    New-Item -ItemType Directory -Force -Path $unitBuild | Out-Null
    Push-Location $unitBuild
    & qmake.exe (Join-Path $root 'tests\tests.pro') 'CONFIG+=release' *> q.log
    & mingw32-make.exe -j8 *> b.log
    $rc = $LASTEXITCODE
    Pop-Location
    if ($rc -ne 0) {
        Write-Host '  [FAIL] 测试套件编译失败，详见 build\mqtt_unit\b.log' -ForegroundColor Red
        Get-Content (Join-Path $unitBuild 'b.log') | Select-String 'error:' | Select-Object -First 8 | ForEach-Object { Write-Host "         $_" }
        $script:Fail = 1; return
    }
    $exe = Join-Path $unitBuild 'release\fieldlink_tests.exe'
    if (-not (Test-Path $exe)) { Write-Host '  [FAIL] 未生成测试可执行文件' -ForegroundColor Red; $script:Fail = 1; return }

    Push-Location (Split-Path $exe)
    $unitArgs = @()
    if ($Fast) { $unitArgs += '--fast' }
    $out = & $exe @unitArgs 2>&1 | ForEach-Object { "$_" }
    Pop-Location

    # 只摘出 MQTT 段，避免刷屏
    $inMqtt = $false
    foreach ($line in $out) {
        if ($line -match '=== Mqtt') { $inMqtt = $true }
        elseif ($line -match '^\s*===' -and $line -notmatch 'Mqtt') { $inMqtt = $false }
        if ($inMqtt) { Write-Host "  $line" }
    }
    $summary = $out | Where-Object { $_ -match '^PASS: ' }
    $summary | ForEach-Object { Write-Host "  全部用例：$_" -ForegroundColor Green }
    if (-not ($summary | Where-Object { $_ -match 'FAIL: 0' })) { $script:Fail = 1 }
}

# ---------- 第 2 层：端到端（外部起 broker） ----------
function Invoke-E2E {
    Write-Head '[2/2] MQTT 端到端测试（真实 broker 子进程）'

    if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
        Write-Host '  [FAIL] 未找到 python（broker 脚本需要）' -ForegroundColor Red
        $script:Fail = 1; return
    }

    $exe = Build-E2E
    if (-not $exe) { return }

    $noAuth = Start-Broker $Port
    if (-not $noAuth) { return }
    $auth = Start-Broker $AuthPort @('--user', 'admin', '--pass', 'secret123')

    Write-Host ''
    Push-Location $root
    & $exe --port $Port 2>&1 | ForEach-Object { Write-Host "  $_" }
    $rc = $LASTEXITCODE
    Pop-Location
    if ($rc -ne 0) { $script:Fail = 1 }

    if ($auth) {
        Write-Host ''
        Write-Host '  --- 认证用例（admin/secret123）---'
        Push-Location $root
        $authOut = & $exe --port $AuthPort --user admin --pass secret123 2>&1 | ForEach-Object { "$_" }
        Pop-Location
        $authOut | Where-Object { $_ -match 'E2E-3' } | ForEach-Object { Write-Host "  $_" }

        # 只判定 E2E-3 段内的结果。该 broker 强制认证，其余段落（E2E-1/2/4/5/6）
        # 本就不带凭据连接，必然失败——那是预期行为，不能算作测试失败。
        $inAuthSection = $false
        $authFails = @()
        foreach ($line in $authOut) {
            if ($line -match '=== E2E-3') { $inAuthSection = $true }
            elseif ($line -match '=== E2E-4') { $inAuthSection = $false }
            if ($inAuthSection -and $line -match '\[FAIL\]') { $authFails += $line }
        }
        if ($authFails.Count -gt 0) {
            Write-Host '  [FAIL] 认证用例存在失败项' -ForegroundColor Red
            $authFails | ForEach-Object { Write-Host "         $_" }
            $script:Fail = 1
        }
        Write-Host '  （其余段落在强制认证的 broker 上无凭据连接失败属预期，已忽略）' -ForegroundColor DarkGray
    }

    # broker 侧确认：消息真的到达了服务端，而不只是客户端自认为成功
    $log = Join-Path $build "broker_$Port.log"
    if (Test-Path $log) {
        $publishes = Get-Content $log | Where-Object { $_ -match 'PUBLISH' }
        Write-Host ''
        Write-Host '  broker 侧收到的 PUBLISH：' -ForegroundColor Yellow
        $publishes | Select-Object -First 12 | ForEach-Object {
            $t = if ($_.Length -gt 140) { $_.Substring(0, 140) } else { $_ }
            Write-Host "    $t"
        }
        Write-Host "  broker 共收到 $($publishes.Count) 条 PUBLISH"
        # E2E-1 发 3 条(qos0/retain/json) + E2E-2 发 1 条(qos1) + E2E-6 发 4 条 = 8 条
        if ($publishes.Count -lt 8) {
            Write-Host "  [FAIL] broker 只收到 $($publishes.Count)/8 条，有消息未送达" -ForegroundColor Red
            $script:Fail = 1
        } else {
            Write-Host '  [OK] 8 条消息全部送达 broker' -ForegroundColor Green
        }
    }

    $errLog = Join-Path $build "broker_$Port.err.log"
    if ((Test-Path $errLog) -and (Get-Item $errLog).Length -gt 0) {
        Write-Host '  [FAIL] broker 侧有异常输出（见下方），可能是编码或线程崩溃' -ForegroundColor Red
        Get-Content $errLog | Select-Object -First 10 | ForEach-Object { Write-Host "         $_" }
        $script:Fail = 1
    }
}

# ---------- 主流程 ----------
try {
    if ($Mode -eq 'manual') {
        Write-Head "手工联调模式：broker 监听 127.0.0.1:$Port"
        $null = Start-Broker $Port
        Write-Host ''
        Write-Host '  1) 启动主程序 build\...\fieldlink.exe' -ForegroundColor Yellow
        Write-Host '  2) 菜单 Advanced -> MQTT Publishing' -ForegroundColor Yellow
        Write-Host "     地址填 127.0.0.1 / 端口 $Port -> Save and connect" -ForegroundColor Yellow
        Write-Host '  3) 本窗口会逐条打印 broker 收到的 PUBLISH' -ForegroundColor Yellow
        Write-Host ''
        Write-Host '  按 Ctrl+C 结束（会一并关闭 broker）'
        Write-Host ''
        Get-Content (Join-Path $build "broker_$Port.log") -Wait
    }
    else {
        if ($Mode -eq 'all' -or $Mode -eq 'unit') { Invoke-Unit }
        if ($Mode -eq 'all' -or $Mode -eq 'e2e') { Invoke-E2E }
    }
}
finally {
    Stop-Brokers
}

Write-Host ''
Write-Host ('-' * 70)
if ($script:Fail -eq 0) {
    Write-Host '   MQTT 测试全部通过' -ForegroundColor Green
} else {
    Write-Host '   存在失败项' -ForegroundColor Red
}
Write-Host ('-' * 70)
exit $script:Fail
