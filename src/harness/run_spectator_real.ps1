param([int]$Seconds=55, [int]$Port=18960, [int]$JoinDelay=8, [string]$Network='15,25,5', [switch]$QuickRetry, [string]$Rematch='0', [switch]$UseCode)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$taskTest=Join-Path $taskRoot '_TEST_MBAACC/SpectatorRegression'
$taskOut=Join-Path $taskRoot ('build_logs/spectator_'+(Get-Date -Format 'yyyyMMdd_HHmmss'))
New-Item -ItemType Directory -Force $taskOut | Out-Null
foreach($taskSide in 1,2,3) {
    $taskGame=Join-Path $taskTest "MBAACC_$taskSide/MBAA.exe"
    if(!(Test-Path -LiteralPath $taskGame)){throw "独立コピーが必要: $taskGame"}
    if(@(Get-CimInstance Win32_Process -Filter "Name='MBAA.exe'" | Where-Object {$_.ExecutablePath -eq $taskGame}).Count){throw '既存ゲームを保全して中止'}
}
$taskBefore=@{}
Get-ChildItem -LiteralPath $taskTest -Filter '*.ini' -Recurse -File | ForEach-Object {$taskBefore[$_.FullName]=(Get-FileHash -LiteralPath $_.FullName).Hash}
$taskBefore | ConvertTo-Json | Set-Content (Join-Path $taskOut 'ini_before.json')
$taskViewer=$null; $taskPair=$null
$taskSavedEnv=@{}
foreach($taskKey in 'CCCASTER_TEST_RETRY_QUICK','CCCASTER_TEST_REMATCH','CCCASTER_TEST_NATIVE_RETRY','CCCASTER_SCRIPT_INPUT','CCCASTER_INPUT_TRACE','CCCASTER_MEM_TRACE','CCCASTER_TEST_NETWORK') {
    $taskSavedEnv[$taskKey]=[Environment]::GetEnvironmentVariable($taskKey,'Process')
}
try {
    if($QuickRetry){$env:CCCASTER_TEST_RETRY_QUICK='1'; $env:CCCASTER_TEST_REMATCH=$Rematch; $env:CCCASTER_TEST_NATIVE_RETRY='1'}
    $taskPairArgs=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'run_bounded_real_pair.ps1'),'-Seconds',$Seconds,'-Port',$Port,'-TestRoot',$taskTest,'-OutputDirectory',(Join-Path $taskOut 'pair'))
    if($Network){$taskPairArgs+=@('-Network',$Network)}
    $taskPair=Start-Process (Get-Process -Id $PID).Path -WindowStyle Hidden -PassThru -ArgumentList $taskPairArgs -RedirectStandardOutput (Join-Path $taskOut 'pair_stdout.log') -RedirectStandardError (Join-Path $taskOut 'pair_stderr.log')
    $taskHostLog=Join-Path $taskTest 'MBAACC_1/cccaster/cccaster_hook_log.txt'
    $taskReady=[DateTime]::UtcNow.AddSeconds(25)
    do {
        Start-Sleep -Milliseconds 200
        if($taskPair.HasExited){throw '対戦側の起動失敗'}
        $taskListening=Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue
    } while(!$taskListening -and [DateTime]::UtcNow -lt $taskReady)
    if(!$taskListening){throw '観戦TCP待受なし'}
    if($JoinDelay){Start-Sleep -Seconds $JoinDelay}
    $taskDir=Join-Path $taskTest 'MBAACC_3/cccaster'
    foreach($taskFile in 'CCCaster_v10.exe','libcccaster_hook.dll','CCCaster_v10_GUI.exe') {
        Copy-Item -LiteralPath (Join-Path $taskRoot "build/bin/$taskFile") -Destination (Join-Path $taskDir $taskFile)
    }
    $taskLog=Join-Path $taskDir 'cccaster_hook_log.txt'
    if(Test-Path -LiteralPath $taskLog){Move-Item -LiteralPath $taskLog -Destination (Join-Path $taskOut 'before_viewer.log')}
    $env:CCCASTER_SCRIPT_INPUT='1'; $env:CCCASTER_INPUT_TRACE='1'; $env:CCCASTER_MEM_TRACE='1'
    Remove-Item Env:CCCASTER_TEST_NETWORK -ErrorAction SilentlyContinue
    $taskViewerArgs="--spectate --ip 127.0.0.1 --port $Port"
    if($UseCode) {
        $taskHostOutput=Get-Content -Raw (Join-Path $taskOut 'pair/launcher_1.log')
        $taskCode=[regex]::Match($taskHostOutput,'\[SPECTATOR CODE\] (S-[A-Z2-7]+)').Groups[1].Value
        if(!$taskCode){throw 'ホストの観戦コードなし'}
        $taskViewerArgs="--spectate --hash $taskCode"
    }
    $taskViewer=Start-Process (Join-Path $taskDir 'CCCaster_v10.exe') -WindowStyle Hidden -PassThru -WorkingDirectory $taskDir -ArgumentList $taskViewerArgs -RedirectStandardOutput (Join-Path $taskOut 'viewer_launcher.log') -RedirectStandardError (Join-Path $taskOut 'viewer_launcher.err')
    Write-Output "Logs: $taskOut"
    while(!$taskPair.HasExited){Start-Sleep -Milliseconds 500}
    Start-Sleep -Seconds 1
} finally {
    foreach($taskKey in $taskSavedEnv.Keys){[Environment]::SetEnvironmentVariable($taskKey,$taskSavedEnv[$taskKey],'Process')}
    if($taskPair -and !$taskPair.HasExited){$taskPair.WaitForExit()}
    if($taskViewer) {
        Get-CimInstance Win32_Process -Filter "Name='MBAA.exe'" | Where-Object {$_.ParentProcessId -eq $taskViewer.Id} | ForEach-Object {Stop-Process -Id $_.ProcessId -Force}
        if(!$taskViewer.WaitForExit(4000)){Stop-Process -Id $taskViewer.Id -Force}
    }
    $taskLog=Join-Path $taskTest 'MBAACC_3/cccaster/cccaster_hook_log.txt'
    if(Test-Path -LiteralPath $taskLog){Copy-Item -LiteralPath $taskLog -Destination (Join-Path $taskOut 'viewer.log')}
    $taskChanged=@()
    foreach($taskFile in $taskBefore.Keys){if((Get-FileHash -LiteralPath $taskFile).Hash -ne $taskBefore[$taskFile]){$taskChanged+=$taskFile}}
    @{checked=$taskBefore.Count; changed=$taskChanged} | ConvertTo-Json | Set-Content (Join-Path $taskOut 'ini_check.json')
    Write-Output "Finished: $taskOut"
}
