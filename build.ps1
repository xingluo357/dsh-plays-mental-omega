# build.ps1 - 编译 mo-pilot（32 位 DLL / 注入器 / 提示层）
#
#   .\build.ps1                     # 全部产物 → build\
#   .\build.ps1 -Out mo_pilot_v58.dll   # 换输出名（旧 dll 被游戏占用时用）
#   .\build.ps1 -Yrpp D:\src\YRpp-master
#
# 需要：VS 2022（或 2019）带 x86 C++ 工具链；YRpp 头文件见 docs\YRPP-COMPAT.md

[CmdletBinding()]
param(
  [string]$Out = 'mo_pilot.dll',
  [string]$Yrpp = '',
  [string]$Build = 'build'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not $Yrpp) {
  foreach ($cand in @('YRpp-master', '..\YRpp-master', '..\mo-pilot\YRpp-master')) {
    $p = Join-Path $root $cand
    if (Test-Path (Join-Path $p 'TechnoClass.h')) { $Yrpp = $p; break }
  }
}
if (-not $Yrpp -or -not (Test-Path (Join-Path $Yrpp 'TechnoClass.h'))) {
  throw "找不到 YRpp 头文件。下载 https://github.com/Ares-Developers/YRpp 后放到 YRpp-master\，或传 -Yrpp <路径>"
}

# 找 vcvars32.bat
$vcvars = $null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path $vswhere) {
  $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if ($vsPath) { $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars32.bat' }
}
if (-not $vcvars -or -not (Test-Path $vcvars)) {
  foreach ($p in @(
      "$env:ProgramFiles\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat",
      "$env:ProgramFiles\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars32.bat",
      "$env:ProgramFiles\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars32.bat",
      "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars32.bat")) {
    if (Test-Path $p) { $vcvars = $p; break }
  }
}
if (-not $vcvars) { throw "找不到 vcvars32.bat，请确认装了 VS 的「使用 C++ 的桌面开发」工作负载" }

$outDir = Join-Path $root $Build
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

function Invoke-Cl {
  param([string]$ClArgs, [string]$What)
  Write-Host "==> $What" -ForegroundColor Cyan
  $cmd = "call `"$vcvars`" >nul && cl /nologo $ClArgs"
  cmd /c $cmd
  if ($LASTEXITCODE -ne 0) { throw "编译失败：$What (exit $LASTEXITCODE)" }
}

# 常用开关：/utf-8 让中文注释不炸；/DNOMINMAX 见 docs\YRPP-COMPAT.md
$common = '/utf-8 /DNOMINMAX /LD /std:c++20 /EHsc /O2'

Invoke-Cl ("$common /I`"$Yrpp`" /Fo:`"$outDir\\`" /Fe:`"$outDir\$Out`" `"$root\src\mo_dll.cpp`" /link ole32.lib") "mo_pilot DLL ($Out)"
Invoke-Cl ("/utf-8 /DNOMINMAX /std:c++20 /EHsc /O2 /Fo:`"$outDir\\`" /Fe:`"$outDir\inject.exe`" `"$root\src\inject.cpp`"") "inject.exe"
Invoke-Cl ("$common /Fo:`"$outDir\\`" /Fe:`"$outDir\mo_hint.dll`" `"$root\hint\hint_dll.cpp`" /link user32.lib gdi32.lib") "mo_hint.dll"
Invoke-Cl ("/utf-8 /DNOMINMAX /std:c++20 /EHsc /O2 /Fo:`"$outDir\\`" /Fe:`"$outDir\host.exe`" `"$root\hint\tools\host.cpp`" /link user32.lib gdi32.lib") "host.exe（提示层自测宿主）"

Write-Host ""
Write-Host "OK -> $outDir" -ForegroundColor Green
Get-ChildItem -Path $outDir -Include *.dll,*.exe -File -ErrorAction SilentlyContinue | ForEach-Object { "  {0}  {1} bytes" -f $_.Name, $_.Length }
