param([int]$Seconds=45,[int]$Port=17800,[string]$Network='', [string]$TestRoot='', [ValidateRange(0,2)][int]$CloseSide=0, [switch]$DebugSpikes, [switch]$VirtualController, [switch]$ManualInput, [string]$OutputDirectory='', [switch]$UseConnectionCode)
$taskDebugStarted=[DateTime]::UtcNow
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$taskTest=Join-Path $taskRoot 'test/runtime'
if($TestRoot){$taskTest=(Resolve-Path -LiteralPath $TestRoot).Path}
$taskOut=Join-Path $taskRoot ('test/logs/bounded_real_'+(Get-Date -Format 'yyyyMMdd_HHmmss'))
if($OutputDirectory){$taskOut=[IO.Path]::GetFullPath($OutputDirectory)}
New-Item -ItemType Directory -Force $taskOut | Out-Null
$taskLaunchers=@();$taskGames=@();$taskStartedSides=@()
$taskTestEnv=@{}
foreach($taskKey in 'CCCASTER_SCRIPT_INPUT','CCCASTER_INPUT_TRACE','CCCASTER_MEM_TRACE','CCCASTER_TIME_SCALE','CCCASTER_TEST_NETWORK','CCCASTER_TEST_VIRTUAL_PRODUCT') {
    $taskTestEnv[$taskKey]=[Environment]::GetEnvironmentVariable($taskKey,'Process')
}
# 両側とも起動していないことを、配布ファイルやログに触れる前に確認する。
foreach($taskSide in 1,2) {
    $taskGamePath=Join-Path $taskTest "MBAACC_$taskSide\MBAA.exe"
    if(!(Test-Path -LiteralPath $taskGamePath)){throw "ゲームがない: $taskGamePath"}
    if(@(Get-CimInstance Win32_Process -Filter "Name='MBAA.exe'" | Where-Object {$_.ExecutablePath -eq $taskGamePath}).Count){throw 'テスト対象ゲームが既に起動中。既存プロセスは停止しない。'}
}
$taskOldSceneMerge=$env:CCCASTER_DISABLE_SCENE_MERGE
$taskOldCpuGuard=$env:CCCASTER_DISABLE_GAME_CPU_GUARD
$taskOldSoundPrewarm=$env:CCCASTER_DISABLE_SOUND_PREWARM
$taskOldCpuPin=$env:CCCASTER_GAME_CPU_PIN
$taskOldGameTimer=$env:CCCASTER_LEGACY_GAME_TIMER
$taskOldReplaySaves=$env:CCCASTER_KEEP_CONFIRMED_REPLAY_SNAPSHOTS
$taskOldStartupSeconds=$env:CCCASTER_STARTUP_SECONDS_BASELINE
$taskOldStartupAssets=$env:CCCASTER_STARTUP_ASSETS_BASELINE
$taskOldStartupFirst=$env:CCCASTER_STARTUP_FIRST_BASELINE
try {
    foreach($taskSide in 1,2) {
        $taskDir=Join-Path $taskTest "MBAACC_$taskSide\cccaster_B"
        $taskGamePath=Join-Path $taskTest "MBAACC_$taskSide\MBAA.exe"
        if(!(Test-Path -LiteralPath $taskGamePath)){throw "ゲームがない: $taskGamePath"}
        $taskExisting=@(Get-CimInstance Win32_Process -Filter "Name='MBAA.exe'" | Where-Object {$_.ExecutablePath -eq $taskGamePath})
        if($taskExisting.Count){throw 'テスト対象ゲームが既に起動中。既存プロセスは停止しない。'}
        foreach($taskFile in 'CCCaster_B.exe','libcccaster_hook.dll') {
            $taskDest=Join-Path $taskDir $taskFile
            if(Test-Path -LiteralPath $taskDest){Copy-Item -LiteralPath $taskDest -Destination (Join-Path $taskOut "before_${taskSide}_$taskFile")}
            $taskSource=Join-Path $taskRoot "build\bin\$taskFile"
            if(!(Test-Path -LiteralPath $taskDest) -or (Get-FileHash $taskDest).Hash -ne (Get-FileHash $taskSource).Hash) {
                Copy-Item -LiteralPath $taskSource -Destination $taskDest
            }
            if((Get-FileHash $taskDest).Hash -ne (Get-FileHash (Join-Path $taskRoot "build\bin\$taskFile")).Hash){throw 'デプロイ不一致'}
        }
        $taskLog=Join-Path $taskDir 'cccaster_hook_log.txt'
        if(Test-Path -LiteralPath $taskLog){Move-Item -LiteralPath $taskLog -Destination (Join-Path $taskOut "before_${taskSide}.log")}
    }
    $env:CCCASTER_SCRIPT_INPUT=if($VirtualController -or $ManualInput){'0'}else{'1'};$env:CCCASTER_INPUT_TRACE='1';if(!$VirtualController){Remove-Item Env:CCCASTER_TEST_VIRTUAL_PRODUCT -ErrorAction SilentlyContinue};$env:CCCASTER_MEM_TRACE='1';$env:CCCASTER_TIME_SCALE='1'
    if($Network){$env:CCCASTER_TEST_NETWORK=$Network}else{Remove-Item Env:CCCASTER_TEST_NETWORK -ErrorAction SilentlyContinue}
    foreach($taskSide in 1,2) {
        $taskDir=Join-Path $taskTest "MBAACC_$taskSide\cccaster_B"
        if($VirtualController){$env:CCCASTER_TEST_VIRTUAL_PRODUCT=if($taskSide -eq 1){'05C4054C'}else{'09CC054C'}}
        $taskArgs=if($taskSide -eq 1){"--headless --host --port $Port"}else{"--headless --ip 127.0.0.1 --port $Port"}
        if($UseConnectionCode -and $taskSide -eq 2) {
            $taskCode=''
            $taskCodeDeadline=[DateTime]::UtcNow.AddSeconds(30)
            while([DateTime]::UtcNow -lt $taskCodeDeadline) {
                $taskHostOutput=Get-Content -LiteralPath (Join-Path $taskOut 'launcher_1.log') -Raw -ErrorAction SilentlyContinue
                $taskCodeMatch=[regex]::Match([string]$taskHostOutput,'\[HEADLESS HOST\] Hash: ([A-Za-z0-9]+)')
                if($taskCodeMatch.Success){$taskCode=$taskCodeMatch.Groups[1].Value;break}
                if($taskLaunchers[0].HasExited){throw 'コード発行前に募集側が終了した'}
                Start-Sleep -Milliseconds 100
            }
            if(!$taskCode){throw '接続コードが30秒以内に発行されなかった'}
            $taskArgs="--headless --hash $taskCode"
            "JoinByCode length=$($taskCode.Length) compact=$($taskCode.StartsWith('1'))" |
                Set-Content -LiteralPath (Join-Path $taskOut 'connection_code.txt')
        }
        if($DebugSpikes){$taskArgs+=' --debug-spikes'}
        if($env:CCCASTER_TEST_BASELINE_HOST_SCENE_PAIRS) {
            if($taskSide -eq 1){$env:CCCASTER_DISABLE_SCENE_MERGE='1'}else{Remove-Item Env:CCCASTER_DISABLE_SCENE_MERGE -ErrorAction SilentlyContinue}
        }
        if($env:CCCASTER_TEST_BASELINE_HOST_CPU_GUARD) {
            if($taskSide -eq 1){$env:CCCASTER_DISABLE_GAME_CPU_GUARD='1'}else{Remove-Item Env:CCCASTER_DISABLE_GAME_CPU_GUARD -ErrorAction SilentlyContinue}
        }
        if($env:CCCASTER_TEST_BASELINE_HOST_SOUND_PREWARM) {
            if($taskSide -eq 1){$env:CCCASTER_DISABLE_SOUND_PREWARM='1'}else{Remove-Item Env:CCCASTER_DISABLE_SOUND_PREWARM -ErrorAction SilentlyContinue}
        }
        if($env:CCCASTER_TEST_BASELINE_HOST_STARTUP_SECONDS) {
            if($taskSide -eq 1){$env:CCCASTER_STARTUP_SECONDS_BASELINE='1'}else{Remove-Item Env:CCCASTER_STARTUP_SECONDS_BASELINE -ErrorAction SilentlyContinue}
        }
        if($env:CCCASTER_TEST_BASELINE_HOST_STARTUP_ASSETS) {
            if($taskSide -eq 1){$env:CCCASTER_STARTUP_ASSETS_BASELINE='1'}else{Remove-Item Env:CCCASTER_STARTUP_ASSETS_BASELINE -ErrorAction SilentlyContinue}
        }
        if($env:CCCASTER_TEST_BASELINE_HOST_STARTUP_FIRST) {
            if($taskSide -eq 1){$env:CCCASTER_STARTUP_FIRST_BASELINE='1'}else{Remove-Item Env:CCCASTER_STARTUP_FIRST_BASELINE -ErrorAction SilentlyContinue}
        }
        $taskPin=[Environment]::GetEnvironmentVariable("CCCASTER_TEST_CPU_PIN_$taskSide",'Process')
        if($null -ne $taskPin){$env:CCCASTER_GAME_CPU_PIN=$taskPin}
        elseif($null -eq $taskOldCpuPin){Remove-Item Env:CCCASTER_GAME_CPU_PIN -ErrorAction SilentlyContinue}
        else{$env:CCCASTER_GAME_CPU_PIN=$taskOldCpuPin}
        if($env:CCCASTER_TEST_BASELINE_HOST_REPLAY_SAVES){
            if($taskSide -eq 1){$env:CCCASTER_KEEP_CONFIRMED_REPLAY_SNAPSHOTS='1'}else{Remove-Item Env:CCCASTER_KEEP_CONFIRMED_REPLAY_SNAPSHOTS -ErrorAction SilentlyContinue}
        }
        if($env:CCCASTER_TEST_BASELINE_HOST_GAME_TIMER){
            if($taskSide -eq 1){$env:CCCASTER_LEGACY_GAME_TIMER='1'}else{Remove-Item Env:CCCASTER_LEGACY_GAME_TIMER -ErrorAction SilentlyContinue}
        }
        $taskLaunchers+=Start-Process (Join-Path $taskDir 'CCCaster_B.exe') -WindowStyle Hidden -PassThru -WorkingDirectory $taskDir -ArgumentList $taskArgs -RedirectStandardOutput (Join-Path $taskOut "launcher_$taskSide.log") -RedirectStandardError (Join-Path $taskOut "launcher_$taskSide.err")
        $taskStartedSides+=$taskSide
        if($taskSide -eq 1){Start-Sleep -Seconds 3}
    }
    Write-Output "Logs: $taskOut"
    $taskDeadline=[DateTime]::UtcNow.AddSeconds($Seconds)
    while([DateTime]::UtcNow -lt $taskDeadline) {
        $taskChildren=@(Get-CimInstance Win32_Process -Filter "Name='MBAA.exe'" | Where-Object {$_.ParentProcessId -in $taskLaunchers.Id})
        foreach($taskChild in $taskChildren){if($taskChild.ProcessId -notin $taskGames){$taskGames+=$taskChild.ProcessId}}
        if(@($taskLaunchers | Where-Object {!$_.HasExited}).Count -eq 0){break}
        Start-Sleep -Milliseconds 500
    }
    if($CloseSide) {
        $taskChildren=@(Get-CimInstance Win32_Process -Filter "Name='MBAA.exe'" | Where-Object {$_.ParentProcessId -in $taskLaunchers.Id})
        $taskVictim=@($taskChildren | Where-Object {$_.ExecutablePath -eq (Join-Path $taskTest "MBAACC_$CloseSide\MBAA.exe")})
        $taskPeer=@($taskChildren | Where-Object {$_.ExecutablePath -eq (Join-Path $taskTest "MBAACC_$(3-$CloseSide)\MBAA.exe")})
        if($taskVictim.Count -ne 1 -or $taskPeer.Count -ne 1){throw '終了試験の両ゲームが揃っていない'}
        $taskPeerProcess=Get-Process -Id $taskPeer[0].ProcessId
        $taskClock=[Diagnostics.Stopwatch]::StartNew()
        Stop-Process -Id $taskVictim[0].ProcessId -Force
        if(!$taskPeerProcess.WaitForExit(2000)){throw '相手が2秒以内に終了しなかった'}
        $taskElapsed=$taskClock.Elapsed.TotalMilliseconds
        Start-Sleep -Milliseconds 300
        $taskPeerLog=Get-Content -Raw (Join-Path $taskOut "launcher_$(3-$CloseSide).log")
        if(!$taskPeerLog.Contains('[ PEER CLOSED ]')){throw '相手終了の通知表示がない'}
        "PeerClose side=$CloseSide elapsedMs=$taskElapsed" | Tee-Object -FilePath (Join-Path $taskOut 'close_result.txt')
    }
} finally {
    foreach($taskKey in $taskTestEnv.Keys){[Environment]::SetEnvironmentVariable($taskKey,$taskTestEnv[$taskKey],'Process')}
    $taskChildren=@(Get-CimInstance Win32_Process -Filter "Name='MBAA.exe'" | Where-Object {$_.ParentProcessId -in $taskLaunchers.Id})
    foreach($taskChild in $taskChildren){if($taskChild.ProcessId -notin $taskGames){$taskGames+=$taskChild.ProcessId}}
    if($null -eq $taskOldReplaySaves){Remove-Item Env:CCCASTER_KEEP_CONFIRMED_REPLAY_SNAPSHOTS -ErrorAction SilentlyContinue}else{$env:CCCASTER_KEEP_CONFIRMED_REPLAY_SNAPSHOTS=$taskOldReplaySaves}
    if($null -eq $taskOldGameTimer){Remove-Item Env:CCCASTER_LEGACY_GAME_TIMER -ErrorAction SilentlyContinue}else{$env:CCCASTER_LEGACY_GAME_TIMER=$taskOldGameTimer}
    if($null -eq $taskOldStartupFirst){Remove-Item Env:CCCASTER_STARTUP_FIRST_BASELINE -ErrorAction SilentlyContinue}else{$env:CCCASTER_STARTUP_FIRST_BASELINE=$taskOldStartupFirst}
    if($null -eq $taskOldStartupAssets){Remove-Item Env:CCCASTER_STARTUP_ASSETS_BASELINE -ErrorAction SilentlyContinue}else{$env:CCCASTER_STARTUP_ASSETS_BASELINE=$taskOldStartupAssets}
    if($null -eq $taskOldStartupSeconds){Remove-Item Env:CCCASTER_STARTUP_SECONDS_BASELINE -ErrorAction SilentlyContinue}else{$env:CCCASTER_STARTUP_SECONDS_BASELINE=$taskOldStartupSeconds}
    if($null -eq $taskOldCpuPin){Remove-Item Env:CCCASTER_GAME_CPU_PIN -ErrorAction SilentlyContinue}else{$env:CCCASTER_GAME_CPU_PIN=$taskOldCpuPin}
    if($null -eq $taskOldSoundPrewarm){Remove-Item Env:CCCASTER_DISABLE_SOUND_PREWARM -ErrorAction SilentlyContinue}else{$env:CCCASTER_DISABLE_SOUND_PREWARM=$taskOldSoundPrewarm}
    if($null -eq $taskOldCpuGuard){Remove-Item Env:CCCASTER_DISABLE_GAME_CPU_GUARD -ErrorAction SilentlyContinue}else{$env:CCCASTER_DISABLE_GAME_CPU_GUARD=$taskOldCpuGuard}
    if($null -eq $taskOldSceneMerge){Remove-Item Env:CCCASTER_DISABLE_SCENE_MERGE -ErrorAction SilentlyContinue}else{$env:CCCASTER_DISABLE_SCENE_MERGE=$taskOldSceneMerge}
    foreach($taskGame in $taskGames){Stop-Process -Id $taskGame -Force -ErrorAction SilentlyContinue}
    foreach($taskLauncher in $taskLaunchers){
        if($DebugSpikes -and !$taskLauncher.HasExited){$null=$taskLauncher.WaitForExit(2000)}
        if(!$taskLauncher.HasExited){Stop-Process -Id $taskLauncher.Id -Force}
    }
    Start-Sleep -Milliseconds 500
    foreach($taskSide in $taskStartedSides) {
        $taskLog=Join-Path $taskTest "MBAACC_$taskSide\cccaster_B\cccaster_hook_log.txt"
        if(Test-Path -LiteralPath $taskLog){Copy-Item -LiteralPath $taskLog -Destination (Join-Path $taskOut "game_$taskSide.log")}
        if($DebugSpikes){
            $taskDebugLogs=@(Get-ChildItem -LiteralPath (Split-Path -Parent $taskLog) -Filter 'spike_debug_*.tsv' |
                Where-Object {$_.LastWriteTimeUtc -ge $taskDebugStarted})
            if(!$taskDebugLogs.Count){throw "デバッガ採取なし: side=$taskSide"}
            $taskDebugLogs | ForEach-Object {Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $taskOut "game_${taskSide}_$($_.Name)")}
        }
    }
    Write-Output "Finished: $taskOut"
}
