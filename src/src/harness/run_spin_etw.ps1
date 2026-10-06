param([ValidateRange(45,120)][int]$Seconds=75, [string]$OutputDirectory='', [string]$Python='python', [switch]$SchedulerOnly)
$ErrorActionPreference='Stop'
$taskIdentity=[Security.Principal.WindowsIdentity]::GetCurrent()
$taskPrincipal=New-Object Security.Principal.WindowsPrincipal($taskIdentity)
if($taskPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){
    throw 'ゲームを通常権限で測るため、この脚本は通常権限のPowerShellから実行してください。採取ヘルパーだけUACで昇格します。'
}
$taskRoot=Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$taskOut=Join-Path $taskRoot ('test/logs/spin_etw_'+(Get-Date -Format 'yyyyMMdd_HHmmss'))
if($OutputDirectory){$taskOut=[IO.Path]::GetFullPath($OutputDirectory)}
$taskAllowed=[IO.Path]::GetFullPath((Join-Path $taskRoot 'test/logs'))+[IO.Path]::DirectorySeparatorChar
if(!$taskOut.StartsWith($taskAllowed,[StringComparison]::OrdinalIgnoreCase)){throw '出力先はこの作業場所のtest/logs内だけです。'}
New-Item -ItemType Directory -Path $taskOut | Out-Null
$taskAdmin=$null
$taskPairExit=1
Write-Output "ETW logs: $taskOut"
try {
    $taskHelper=Join-Path $PSScriptRoot 'record_spin_etw_admin.ps1'
    $taskPowerShell=(Get-Command pwsh -ErrorAction Stop).Source
    $taskArgs='-NoProfile -File "'+$taskHelper+'" -OutputDir "'+$taskOut+'"'
    if($SchedulerOnly){$taskArgs+=' -Profile SchedulerOnly'}
    $taskAdmin=Start-Process -FilePath $taskPowerShell -ArgumentList $taskArgs -Verb RunAs -WindowStyle Hidden -PassThru
    $taskReadyDeadline=[DateTime]::UtcNow.AddSeconds(30)
    while(!(Test-Path -LiteralPath (Join-Path $taskOut 'ready.json'))){
        if(Test-Path -LiteralPath (Join-Path $taskOut 'completed.json')){throw (Get-Content (Join-Path $taskOut 'completed.json') -Raw)}
        if($taskAdmin.HasExited -or [DateTime]::UtcNow -gt $taskReadyDeadline){throw 'ETWヘルパーが開始しませんでした。'}
        Start-Sleep -Milliseconds 200
    }
    & $Python -X utf8 (Join-Path $PSScriptRoot 'run_deadline_diagnostics.py') --seconds $Seconds --detailed --output (Join-Path $taskOut 'pair') |
        Tee-Object -FilePath (Join-Path $taskOut 'pair_output.txt')
    $taskPairExit=$LASTEXITCODE
} finally {
    [IO.File]::WriteAllText((Join-Path $taskOut 'stop.request'),'stop own capture')
    if($taskAdmin){
        $taskStopDeadline=[DateTime]::UtcNow.AddSeconds(120)
        while(!(Test-Path -LiteralPath (Join-Path $taskOut 'completed.json')) -and !$taskAdmin.HasExited -and [DateTime]::UtcNow -lt $taskStopDeadline){Start-Sleep -Milliseconds 200}
    }
}
$taskCompletion=Join-Path $taskOut 'completed.json'
if(!(Test-Path -LiteralPath $taskCompletion)){throw 'ETW採取の終了結果がありません。'}
$taskResult=Get-Content -LiteralPath $taskCompletion -Raw | ConvertFrom-Json
if(!$taskResult.started -or !$taskResult.stopped -or $taskResult.error){throw ($taskResult | ConvertTo-Json)}
if(!(Test-Path -LiteralPath (Join-Path $taskOut 'scheduler.etl'))){throw "ETL未保存: $taskOut"}
if($taskPairExit -ne 0){throw "実対戦診断が不合格: $taskPairExit"}
Write-Output "ETW finished: $taskOut"
