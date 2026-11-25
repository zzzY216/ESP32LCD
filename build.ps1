# ===================================================================
#  build.ps1 -- ESP32 项目构建/烧录辅助脚本
#
#  用法（在项目根目录 D:\esp32\MyProject\OLCD 下执行）：
#      .\build.ps1              # 构建
#      .\build.ps1 -Flash       # 构建并烧录
#      .\build.ps1 -Monitor     # 构建、烧录并打开串口监视器
#      .\build.ps1 -Clean       # 清理后重新构建
#      .\build.ps1 -Menuconfig  # 打开配置界面
#
#  为什么需要这个脚本：
#      EIM 安装的 ESP-IDF 激活脚本没有设置 ESP_IDF_VERSION，
#      直接调用 idf.py 会报 TypeError: expected string or bytes-like object。
#      另外激活脚本也不把 cmake/ninja/工具链加进 PATH。
#      本脚本补齐这两类环境后再调用 idf.py。
#
#  【重要】目标芯片：本项目运行在 ESP32-S3 上（不是经典 ESP32）
#     从启动日志可确认：ESP-ROM:esp32s3-20210327
#     脚本默认 -Target esp32s3，并在 sdkconfig 已存在时校验一致性。
#
#  【重要】修改 sdkconfig.defaults 后必须删除 sdkconfig 才生效：
#     规则是「sdkconfig 存在时忽略 sdkconfig.defaults」。
#     若只改 defaults 而不删 sdkconfig，改动不会进入固件，
#     日志里的 compile time 会暴露这一点。
# ===================================================================

param(
    [switch]$Flash,
    [switch]$Monitor,
    [switch]$Clean,
    [switch]$Menuconfig,
    [string]$Port = "",
    # 【重要】默认目标芯片必须是 esp32s3，不是 esp32！
    #
    # 之前这里写的是 "esp32"，是个定时炸弹：
    # 一旦删除 sdkconfig（比如为了改 sdkconfig.defaults 生效），
    # 下次运行本脚本就会执行 `idf.py set-target esp32`，
    # 编译出普通 ESP32 固件烧到 ESP32-S3 上 —— 完全无法启动，
    # 而且现象很像硬件故障，极难排查。
    [string]$Target = "esp32s3"
)

$ErrorActionPreference = "Stop"

# ---------- ESP-IDF 环境 ----------
$IdfPath       = "C:\esp\v6.1\esp-idf"
$IdfToolsPath  = "C:\Espressif\tools"
$IdfPythonEnv  = "C:\Espressif\tools\python\v6.1\venv"
$IdfVersion    = "6.1.0"

$env:IDF_PATH              = $IdfPath
$env:IDF_TOOLS_PATH        = $IdfToolsPath
$env:IDF_PYTHON_ENV_PATH   = $IdfPythonEnv
$env:ESP_IDF_VERSION       = $IdfVersion
$env:ESP_ROM_ELF_DIR       = "$IdfToolsPath\esp-rom-elfs\20241011\"
$env:IDF_CCACHE_ENABLE     = "1"

# Windows 控制台默认是 GBK，Python 输出中文/特殊字符会报
# "Your environment is not configured to handle Unicode characters"。
# 强制走 UTF-8 可消除该警告。
$env:PYTHONIOENCODING      = "utf-8"
$env:PYTHONUTF8            = "1"

# ---------- 把构建工具加入 PATH ----------
# EIM 安装的 ESP-IDF 激活脚本只设置环境变量，不把 cmake/ninja/工具链
# 加进 PATH，所以直接调 idf.py 会报 '"cmake" must be available on the PATH'。
# 这里显式补齐。
$toolPaths = @(
    "$IdfToolsPath\cmake\4.0.3\bin",
    "$IdfToolsPath\ninja\1.12.1",
    "$IdfToolsPath\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin",
    "$IdfToolsPath\ccache",
    "$IdfPythonEnv\Scripts"
)

foreach ($p in $toolPaths) {
    if (Test-Path $p) {
        if ($env:PATH -notlike "*$p*") {
            $env:PATH = "$p;$env:PATH"
        }
    } else {
        Write-Host "警告：工具路径不存在，已跳过：$p" -ForegroundColor Yellow
    }
}

$Python = "$IdfPythonEnv\Scripts\python.exe"
$IdfPy  = "$IdfPath\tools\idf.py"

if (-not (Test-Path $Python)) {
    Write-Host "错误：找不到 Python 环境：$Python" -ForegroundColor Red
    Write-Host "请确认 ESP-IDF 已通过 EIM 正确安装。" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $IdfPy)) {
    Write-Host "错误：找不到 idf.py：$IdfPy" -ForegroundColor Red
    exit 1
}

function Invoke-Idf {
    param([string[]]$Arguments)

    # 【为什么要临时放开 ErrorActionPreference】
    #
    # ESP-IDF 构建时会向 stderr 打印大量 CMake Warning，例如：
    #   Private include directory '...esp_wifi/...' belongs to
    #   component esp_wifi but is being used by component wpa_supplicant
    #
    # 这些都是 IDF 内部的组件引用警告，**不影响编译结果**。
    # 但 $ErrorActionPreference = "Stop" 会把任何 stderr 输出当成
    # 终止性错误，导致构建明明成功（exit=0）却被脚本判定为失败。
    #
    # 所以这里临时改为 Continue，只以 idf.py 的真实退出码为准。
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $Python $IdfPy @Arguments
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prevEap
    }

    if ($code -ne 0) {
        Write-Host "idf.py $($Arguments -join ' ') 失败 (exit=$code)" -ForegroundColor Red
        exit $code
    }
}

Write-Host "==========================================" -ForegroundColor Cyan
Write-Host " ESP32 项目构建脚本" -ForegroundColor Cyan
Write-Host " IDF_PATH   : $IdfPath" -ForegroundColor Gray
Write-Host " Python env : $IdfPythonEnv" -ForegroundColor Gray
Write-Host " Target     : $Target" -ForegroundColor Gray
Write-Host "==========================================" -ForegroundColor Cyan

# ---------- 首次构建：设置目标芯片 ----------
$sdkconfig = Join-Path $PWD "sdkconfig"
if (-not (Test-Path $sdkconfig)) {
    Write-Host "`n[1/3] 首次构建，设置目标芯片 $Target ..." -ForegroundColor Yellow
    Invoke-Idf @("set-target", $Target)
} else {
    # sdkconfig 已存在：检查里面记录的 target 与期望是否一致
    # 不一致会导致烧录后完全无法启动，必须拦住
    $found = (Select-String -Path $sdkconfig -Pattern '^CONFIG_IDF_TARGET="(.+)"' |
              Select-Object -First 1).Matches.Groups[1].Value

    if ([string]::IsNullOrEmpty($found)) {
        $found = "(无法解析)"
    }

    if ($found -ne $Target) {
        Write-Host ""
        Write-Host "==========================================================" -ForegroundColor Red
        Write-Host " 错误：目标芯片不匹配" -ForegroundColor Red
        Write-Host "==========================================================" -ForegroundColor Red
        Write-Host " 现有 sdkconfig 记录的芯片 : $found" -ForegroundColor Yellow
        Write-Host " 本脚本期望的芯片         : $Target" -ForegroundColor Yellow
        Write-Host ""
        Write-Host " 继续构建会得到错误芯片的固件，烧录后无法启动。" -ForegroundColor Red
        Write-Host ""
        Write-Host " 如果确实是 $Target 芯片，请先删除 sdkconfig 再运行：" -ForegroundColor Cyan
        Write-Host "     Remove-Item sdkconfig" -ForegroundColor White
        Write-Host "     .\build.ps1" -ForegroundColor White
        Write-Host ""
        Write-Host " 如果芯片其实是 $found，请加参数：" -ForegroundColor Cyan
        Write-Host "     .\build.ps1 -Target $found" -ForegroundColor White
        Write-Host "==========================================================" -ForegroundColor Red
        exit 1
    }

    Write-Host "`n[1/3] sdkconfig 已存在，目标芯片 $found 校验通过" -ForegroundColor Gray
}

# ---------- 清理 ----------
if ($Clean) {
    Write-Host "`n[清理] fullclean ..." -ForegroundColor Yellow
    Invoke-Idf @("fullclean")
}

# ---------- menuconfig ----------
if ($Menuconfig) {
    Write-Host "`n打开配置界面 ..." -ForegroundColor Yellow
    Invoke-Idf @("menuconfig")
    Write-Host "配置已保存，退出。" -ForegroundColor Green
    exit 0
}

# ---------- 构建 ----------
Write-Host "`n[2/3] 编译 ..." -ForegroundColor Yellow
Invoke-Idf @("build")

# ---------- 烧录 ----------
if ($Flash -or $Monitor) {
    Write-Host "`n[3/3] 烧录 ..." -ForegroundColor Yellow
    $flashArgs = @("-p", $Port, "flash")
    if ([string]::IsNullOrEmpty($Port)) {
        $flashArgs = @("flash")
    }
    Invoke-Idf $flashArgs
}

if ($Monitor) {
    Write-Host "`n打开串口监视器（Ctrl+] 退出）..." -ForegroundColor Yellow
    $monArgs = @("monitor")
    if (-not [string]::IsNullOrEmpty($Port)) {
        $monArgs = @("-p", $Port, "monitor")
    }
    Invoke-Idf $monArgs
}

Write-Host "`n==========================================" -ForegroundColor Green
Write-Host " 完成" -ForegroundColor Green
Write-Host "==========================================" -ForegroundColor Green
if (-not $Flash -and -not $Monitor) {
    Write-Host ""
    Write-Host "烧录命令：" -ForegroundColor Cyan
    Write-Host "  .\build.ps1 -Flash" -ForegroundColor White
    Write-Host "  或指定端口：" -ForegroundColor Cyan
    Write-Host "  .\build.ps1 -Flash -Port COM3" -ForegroundColor White
}
