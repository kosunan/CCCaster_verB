#Requires -RunAsAdministrator
param([Parameter(Mandatory=$true)][string]$OutputDir,
      [ValidateSet('SpinScheduler','SchedulerOnly')][string]$Profile='SpinScheduler')
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$taskAllowed=[IO.Path]::GetFullPath((Join-Path $taskRoot 'test/logs'))+[IO.Path]::DirectorySeparatorChar
$taskOut=(Resolve-Path -LiteralPath $OutputDir).Path
if(!$taskOut.StartsWith($taskAllowed,[StringComparison]::OrdinalIgnoreCase)){throw '出力先はこの作業場所のtest/logs内だけです。'}
# 昇格側はリンク先への書込みや過去の採取への上書きを受け付けない。
$taskParent=Get-Item -LiteralPath $taskOut
while($taskParent){
    if($taskParent.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'リンクを含む出力先は使えません。'}
    $taskParent=$taskParent.Parent
}
foreach($taskNameInUse in 'ready.json','completed.json','scheduler.etl','wpr_temp'){
    if(Test-Path -LiteralPath (Join-Path $taskOut $taskNameInUse)){throw '既存の採取先は使えません。'}
}
$taskName='CCCasterSpin_'+[Guid]::NewGuid().ToString('N')
$taskStarted=$false
$taskResult=[ordered]@{instance=$taskName;profile=$Profile;started=$false;stopped=$false;error=$null}
try {
    $taskProfile=Join-Path $PSScriptRoot 'spin_scheduler.wprp'
    $taskTemp=Join-Path $taskOut 'wpr_temp'
    New-Item -ItemType Directory -Path $taskTemp | Out-Null
    & wpr -start "$taskProfile!$Profile" -filemode -recordtempto $taskTemp -instancename $taskName > (Join-Path $taskOut 'wpr_start.txt') 2>&1
    if($LASTEXITCODE -ne 0){throw "WPR開始失敗: $LASTEXITCODE"}
    $taskStarted=$true
    $taskResult.started=$true
    [IO.File]::WriteAllText((Join-Path $taskOut 'ready.json'),($taskResult | ConvertTo-Json))
    $taskDeadline=[DateTime]::UtcNow.AddSeconds(210)
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
