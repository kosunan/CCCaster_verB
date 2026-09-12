#Requires -RunAsAdministrator
param([Parameter(Mandatory=$true)][string]$OutputDir)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$taskAllowed=[IO.Path]::GetFullPath((Join-Path $taskRoot 'build_logs'))+[IO.Path]::DirectorySeparatorChar
$taskOut=(Resolve-Path -LiteralPath $OutputDir).Path
if(!$taskOut.StartsWith($taskAllowed,[StringComparison]::OrdinalIgnoreCase)){throw '出力先はこの作業場所のbuild_logs内だけです。'}
$taskName='CCCasterSpin_'+[Guid]::NewGuid().ToString('N')
$taskStarted=$false
$taskResult=[ordered]@{instance=$taskName;started=$false;stopped=$false;error=$null}
try {
    $taskProfile=Join-Path $PSScriptRoot 'spin_scheduler.wprp'
    $taskTemp=Join-Path $taskOut 'wpr_temp'
    New-Item -ItemType Directory -Path $taskTemp | Out-Null
    & wpr -start "$taskProfile!SpinScheduler" -filemode -recordtempto $taskTemp -instancename $taskName > (Join-Path $taskOut 'wpr_start.txt') 2>&1
    if($LASTEXITCODE -ne 0){throw "WPR開始失敗: $LASTEXITCODE"}
    $taskStarted=$true
    $taskResult.started=$true
    [IO.File]::WriteAllText((Join-Path $taskOut 'ready.json'),($taskResult | ConvertTo-Json))
    $taskDeadline=[DateTime]::UtcNow.AddSeconds(150)
    while([DateTime]::UtcNow -lt $taskDeadline -and !(Test-Path -LiteralPath (Join-Path $taskOut 'stop.request'))) {
        Start-Sleep -Milliseconds 200
    }
} catch {
    $taskResult.error=$_.Exception.Message
} finally {
    if($taskStarted) {
        try {
            & wpr -status collectors -details -instancename $taskName > (Join-Path $taskOut 'wpr_status.txt') 2>&1
            & wpr -stop (Join-Path $taskOut 'scheduler.etl') -skipPdbGen -instancename $taskName > (Join-Path $taskOut 'wpr_stop.txt') 2>&1
            if($LASTEXITCODE -ne 0){throw "WPR保存失敗: $LASTEXITCODE。対象instance=$taskName"}
            $taskResult.stopped=$true
        } catch { $taskResult.error=$_.Exception.Message }
    }
    [IO.File]::WriteAllText((Join-Path $taskOut 'completed.json'),($taskResult | ConvertTo-Json))
}
if($taskResult.error){exit 1}
