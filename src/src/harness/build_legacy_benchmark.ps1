$ErrorActionPreference = 'Stop'
$benchRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$benchOutput = Join-Path $benchRoot 'test/tools/frame_observer'
$benchMinhook = Join-Path $benchRoot 'build/_deps/minhook-src'
$env:PATH = 'C:/msys64/mingw32/bin;' + $env:PATH
New-Item -ItemType Directory -Force -Path $benchOutput | Out-Null
$benchObjects = @()
foreach ($benchUnit in @('buffer', 'hook', 'trampoline', 'hde/hde32')) {
    $benchObject = Join-Path $benchOutput (($benchUnit -replace '/', '_') + '.o')
    & gcc -m32 -O2 '-I' (Join-Path $benchMinhook 'include') '-I' (Join-Path $benchMinhook 'src') '-c' (Join-Path $benchMinhook ('src/' + $benchUnit + '.c')) '-o' $benchObject
    if ($LASTEXITCODE) { throw "MinHookのビルド失敗: $benchUnit" }
    $benchObjects += $benchObject
}
& g++ -m32 -std=c++20 -O2 -shared -static '-I' (Join-Path $benchRoot 'src/src') '-I' (Join-Path $benchMinhook 'include') (Join-Path $PSScriptRoot 'legacy_benchmark_probe.cpp') @benchObjects '-o' (Join-Path $benchOutput 'legacy_benchmark_probe.dll')
if ($LASTEXITCODE) { throw '共通観測DLLのビルド失敗' }
& g++ -m32 -std=c++20 -O2 -static -municode '-I' (Join-Path $benchRoot 'src/src') (Join-Path $PSScriptRoot 'legacy_benchmark_inject.cpp') '-o' (Join-Path $benchOutput 'legacy_benchmark_inject.exe')
if ($LASTEXITCODE) { throw '注入補助のビルド失敗' }
Write-Output "32bit観測ツール: $benchOutput"
