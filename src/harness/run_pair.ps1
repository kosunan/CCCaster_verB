# ============================================================================
# run_pair.ps1 — ハーネスを2プロセス起動して loopback UDP で繋ぐ
#
#   .\src\harness\run_pair.ps1
#   .\src\harness\run_pair.ps1 -HostLoadingFrames 60 -ClientLoadingFrames 180
#
# ロード時間を左右で変えると、証言②（ロード時間のばらつきでずれる）を再現できる。
# 記録は build_logs/harness/ に出力される。
# ============================================================================
param(
    [int]    $HostLoadingFrames   = 60,
    [int]    $ClientLoadingFrames = 60,
    [int]    $Rounds              = 2,
    [int]    $HostPort            = 7600,
    [int]    $ClientPort          = 7601,
    [int]    $TimeoutSeconds      = 90,
    [int]    $TimeScale = 1           # 時間圧縮。4 なら4倍速（区切りの確認は必ず 1 で）
)

$ErrorActionPreference = 'SilentlyContinue'
$root    = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe     = Join-Path $root 'build\bin\harness.exe'
$outDir  = Join-Path $root 'build_logs\harness'

if (-not (Test-Path $exe)) {
    Write-Output "harness.exe がありません: $exe"
    exit 1
}
New-Item -ItemType Directory -Force $outDir | Out-Null

$env:CCCASTER_TIME_SCALE = "$TimeScale"
if ($TimeScale -gt 1) { Write-Output "[pair] 時間圧縮 x$TimeScale（タイミング余裕の検証にはならない）" }

Get-Process -Name 'harness' | Stop-Process -Force
Start-Sleep -Milliseconds 300

$hostRec   = Join-Path $outDir 'host_record.txt'
$clientRec = Join-Path $outDir 'client_record.txt'
$hostLog   = Join-Path $outDir 'host.log'
$clientLog = Join-Path $outDir 'client.log'

Write-Output "[pair] HOST   loading=${HostLoadingFrames}F  local=$HostPort   peer=$ClientPort"
Write-Output "[pair] CLIENT loading=${ClientLoadingFrames}F  local=$ClientPort  peer=$HostPort"

$h = Start-Process -FilePath $exe -PassThru -NoNewWindow -RedirectStandardOutput $hostLog `
    -ArgumentList '--host','--ip','127.0.0.1',
                  '--port',$ClientPort,'--local-port',$HostPort,
                  '--loading-frames',$HostLoadingFrames,'--rounds',$Rounds,
                  '--out',$hostRec

Start-Sleep -Milliseconds 500

$c = Start-Process -FilePath $exe -PassThru -NoNewWindow -RedirectStandardOutput $clientLog `
    -ArgumentList '--ip','127.0.0.1',
                  '--port',$HostPort,'--local-port',$ClientPort,
                  '--loading-frames',$ClientLoadingFrames,'--rounds',$Rounds,
                  '--out',$clientRec

Write-Output "[pair] 起動しました。最大 ${TimeoutSeconds} 秒待機します..."
$done = Wait-Process -Id $h.Id,$c.Id -Timeout $TimeoutSeconds -PassThru -ErrorAction SilentlyContinue
Get-Process -Name 'harness' | Stop-Process -Force

Write-Output ''
Write-Output '===== HOST (末尾8行) ====='
if (Test-Path $hostLog)   { Get-Content $hostLog   -Tail 8 }
Write-Output ''
Write-Output '===== CLIENT (末尾8行) ====='
if (Test-Path $clientLog) { Get-Content $clientLog -Tail 8 }

Write-Output ''
Write-Output '===== 決定性チェック ====='

if (-not (Test-Path $hostRec) -or -not (Test-Path $clientRec)) {
    Write-Output '  記録が揃っていません'
    Write-Output 'DONE'
    exit 1
}

# netFrame → "p1dir p1btn p2dir p2btn" の対応表を作る。
# 両プロセスは進行がずれるので、共通する netFrame だけを突き合わせる。
function Read-Record($path) {
    $map = @{}
    foreach ($line in Get-Content $path) {
        if ($line -match '^#') { continue }
        $parts = $line.Split(' ')
        if ($parts.Count -lt 5) { continue }
        $map[[uint32]$parts[0]] = ($parts[1..4] -join ' ')
    }
    return $map
}

$h = Read-Record $hostRec
$c = Read-Record $clientRec
Write-Output ("  host  : {0} フレーム記録" -f $h.Count)
Write-Output ("  client: {0} フレーム記録" -f $c.Count)

$common = $h.Keys | Where-Object { $c.ContainsKey($_) } | Sort-Object
$mismatch = @()
foreach ($f in $common) {
    if ($h[$f] -ne $c[$f]) { $mismatch += $f }
}

Write-Output ("  共通フレーム: {0}" -f $common.Count)
if ($common.Count -eq 0) {
    Write-Output '  [NG] 突き合わせ可能なフレームがありません'
} elseif ($mismatch.Count -eq 0) {
    Write-Output '  [OK] 共通フレームの入力列は完全に一致'
} else {
    Write-Output ("  [NG] {0} フレームで不一致。先頭5件:" -f $mismatch.Count)
    foreach ($f in ($mismatch | Select-Object -First 5)) {
        Write-Output ("    netFrame={0}  host=[{1}]  client=[{2}]" -f $f, $h[$f], $c[$f])
    }
}

Write-Output ''
Write-Output '===== ゲーム状態の突き合わせ ====='
# 実機の [MEM] 判定と同じ形。同じ netFrame で両者のゲーム状態が揃っているか。
# 入力列が一致していても、ここがずれていれば実際の対戦はデシンクする。
function Read-State($path) {
    $map = @{}
    if (-not (Test-Path $path)) { return $map }
    foreach ($line in Get-Content $path) {
        if ($line -match '^#') { continue }
        $a = $line.Split(' ')
        if ($a.Count -lt 5) { continue }
        $f = [uint32]$a[0]
        if (-not $map.ContainsKey($f)) { $map[$f] = $a }   # 同一 netFrame は最初を採用
    }
    return $map
}
$sh = Read-State "$hostRec.state"
$sc = Read-State "$clientRec.state"

if ($sh.Count -eq 0 -or $sc.Count -eq 0) {
    Write-Output '  状態記録がありません'
} else {
    $cols = @('mode','intro','WT','RT')
    $skeys = $sh.Keys | Where-Object { $sc.ContainsKey($_) } | Sort-Object
    Write-Output ("  共通 netFrame: {0}" -f $skeys.Count)
    foreach ($i in 0..($cols.Count-1)) {
        $idx = $i + 1
        $first = $null
        foreach ($k in $skeys) {
            if ($sh[$k][$idx] -ne $sc[$k][$idx]) { $first = $k; break }
        }
        if ($null -eq $first) {
            Write-Output ("    {0,-6}: 一致" -f $cols[$i])
        } else {
            Write-Output ("    {0,-6}: netFrame={1} で分岐 (host={2} client={3})" -f `
                $cols[$i], $first, $sh[$first][$idx], $sc[$first][$idx])
        }
    }
}

Write-Output ''
Write-Output '===== stall / conflict ====='
foreach ($p in @(@('host',$hostLog), @('client',$clientLog))) {
    $last = Get-Content $p[1] | Where-Object { $_ -match 'stall=' } | Select-Object -Last 1
    Write-Output ("  {0}: {1}" -f $p[0], $last)
}
Write-Output 'DONE'
