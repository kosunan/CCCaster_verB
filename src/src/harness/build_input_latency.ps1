param([ValidateSet('input_latency','stock_input')][string]$Probe = 'input_latency')
$ErrorActionPreference = 'Stop'
$latencyRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$latencyOutput = Join-Path $latencyRoot 'test/tools/frame_observer'
$latencyMinhook = Join-Path $latencyRoot 'build/_deps/minhook-src'
$env:PATH = 'C:/msys64/mingw32/bin;' + $env:PATH
New-Item -ItemType Directory -Force -Path $latencyOutput | Out-Null
$latencyObjects = @()
foreach ($unit in @('buffer', 'hook', 'trampoline', 'hde/hde32')) {
    $objectPath = Join-Path $latencyOutput (($unit -replace '/', '_') + '.o')
    & gcc -m32 -O2 -I (Join-Path $latencyMinhook 'include') -I (Join-Path $latencyMinhook 'src') -c (Join-Path $latencyMinhook ('src/' + $unit + '.c')) -o $objectPath
    if ($LASTEXITCODE) { throw "観測器MinHookのビルド失敗: $unit" }
    $latencyObjects += $objectPath
}
$latencySources = @("src/src/harness/${Probe}_probe.cpp", 'src/src/core_dll/mbaa_mem/StartupDirectEntry.cpp', 'src/src/core_dll/mbaa_mem/StartupFileRead.cpp') | ForEach-Object { Join-Path $latencyRoot $_ }
& g++ -m32 -std=c++20 -O2 -shared -static -I (Join-Path $latencyRoot 'src/src') -I (Join-Path $latencyMinhook 'include') @latencySources @latencyObjects -ldinput8 -ldxguid -o (Join-Path $latencyOutput "${Probe}_probe.dll")
if ($LASTEXITCODE) { throw '入力遅延観測DLLのビルド失敗' }
& g++ -m32 -std=c++20 -O2 -static -municode -I (Join-Path $latencyRoot 'src/src') (Join-Path $PSScriptRoot 'legacy_benchmark_inject.cpp') -o (Join-Path $latencyOutput 'legacy_benchmark_inject.exe')
if ($LASTEXITCODE) { throw '観測器注入補助のビルド失敗' }
