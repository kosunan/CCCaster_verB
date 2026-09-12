param([ValidateRange(35,75)][int]$Seconds=45,[int]$Port=17860,[string]$Network='15,25,5')
$ErrorActionPreference='Stop'
$taskIdentity=[Security.Principal.WindowsIdentity]::GetCurrent()
$taskPrincipal=New-Object Security.Principal.WindowsPrincipal($taskIdentity)
if($taskPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){
    throw 'ゲームを通常権限で測るため、この脚本は通常権限のPowerShellから実行してください。採取ヘルパーだけUACで昇格します。'
}
$taskRoot=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$taskOut=Join-Path $taskRoot ('build_logs\spin_etw_'+(Get-Date -Format 'yyyyMMdd_HHmmss'))
New-Item -ItemType Directory -Path $taskOut | Out-Null
$taskVars=@('CCCASTER_SPIN_PROBE','CCCASTER_RENDER_PROBE','CCCASTER_PACE_TRACE','CCCASTER_FRAME_TIMING_TRACE','CCCASTER_TEST_BASELINE_HOST_SCENE_PAIRS')
$taskOld=@{}
foreach($taskVar in $taskVars){$taskOld[$taskVar]=[Environment]::GetEnvironmentVariable($taskVar,'Process')}
$taskAdmin=$null
Write-Output "ETW logs: $taskOut"
try {
    if(Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue){throw '検証ポートは使用中です。'}
    foreach($taskSide in 1,2){
        $taskGame=Join-Path $taskRoot "_TEST_MBAACC\MBAACC_$taskSide\MBAA.exe"
        if(!(Test-Path -LiteralPath $taskGame)){throw "ゲームがありません: $taskGame"}
        if(Get-CimInstance Win32_Process -Filter "Name='MBAA.exe'" | Where-Object {$_.ExecutablePath -eq $taskGame}){
            throw '対象ゲームは既に実行中です。既存プロセスは停止しません。'
        }
    }
    $taskHelper=Join-Path $PSScriptRoot 'record_spin_etw_admin.ps1'
    $taskPowerShell=(Get-Command pwsh -ErrorAction Stop).Source
    $taskArgs='-NoProfile -File "'+$taskHelper+'" -OutputDir "'+$taskOut+'"'
    $taskAdmin=Start-Process -FilePath $taskPowerShell -ArgumentList $taskArgs -Verb RunAs -WindowStyle Hidden -PassThru
    $taskReadyDeadline=[DateTime]::UtcNow.AddSeconds(30)
    while(!(Test-Path -LiteralPath (Join-Path $taskOut 'ready.json'))){
        if(Test-Path -LiteralPath (Join-Path $taskOut 'completed.json')){throw (Get-Content (Join-Path $taskOut 'completed.json') -Raw)}
        if($taskAdmin.HasExited -or [DateTime]::UtcNow -gt $taskReadyDeadline){throw 'ETWヘルパーが開始しませんでした。'}
        Start-Sleep -Milliseconds 200
    }
    # PowerShell 7.6では.NETへのnull引数が空文字になり、C++のgetenvで有効扱いになる。
    foreach($taskVar in $taskVars){Remove-Item -LiteralPath "Env:$taskVar" -ErrorAction SilentlyContinue}
    $env:CCCASTER_SPIN_PROBE='1'
    & (Join-Path $PSScriptRoot 'run_bounded_real_pair.ps1') -Seconds $Seconds -Port $Port -Network $Network |
        Tee-Object -FilePath (Join-Path $taskOut 'pair_output.txt')
} finally {
    [IO.File]::WriteAllText((Join-Path $taskOut 'stop.request'),'stop own capture')
    foreach($taskVar in $taskVars){
        if($null -eq $taskOld[$taskVar]){Remove-Item -LiteralPath "Env:$taskVar" -ErrorAction SilentlyContinue}
        else{[Environment]::SetEnvironmentVariable($taskVar,$taskOld[$taskVar],'Process')}
    }
    if($taskAdmin){
        $taskStopDeadline=[DateTime]::UtcNow.AddSeconds(120)
        while(!(Test-Path -LiteralPath (Join-Path $taskOut 'completed.json')) -and !$taskAdmin.HasExited -and [DateTime]::UtcNow -lt $taskStopDeadline){Start-Sleep -Milliseconds 200}
        if(Test-Path -LiteralPath (Join-Path $taskOut 'completed.json')){Get-Content (Join-Path $taskOut 'completed.json')}
    }
}
if(!(Test-Path -LiteralPath (Join-Path $taskOut 'scheduler.etl'))){throw "ETL未保存: $taskOut"}
Write-Output "ETW finished: $taskOut"
