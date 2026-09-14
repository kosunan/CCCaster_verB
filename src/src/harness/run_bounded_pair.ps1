param([int]$Scale=4,[int]$Rounds=1,[int]$HostLoading=60,[int]$ClientLoading=180,[int]$Port=17700,[string]$Name='gate_pair',[string]$Network='',[int]$HostStall=0)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$taskOut=Join-Path $taskRoot ('test\logs\'+$Name)
New-Item -ItemType Directory -Force $taskOut | Out-Null
$env:PATH='C:\msys64\mingw32\bin;'+$env:PATH
$env:CCCASTER_TIME_SCALE="$Scale"
$env:CCCASTER_SCRIPT_INPUT='1'
if($Network){$env:CCCASTER_TEST_NETWORK=$Network}else{Remove-Item Env:CCCASTER_TEST_NETWORK -ErrorAction SilentlyContinue}
$taskProcesses=@()
try {
    foreach($taskRole in 'host','client') {
        $taskLocal=if($taskRole -eq 'host'){$Port}else{$Port+1}
        $taskPeer=if($taskRole -eq 'host'){$Port+1}else{$Port}
        $taskLoading=if($taskRole -eq 'host'){$HostLoading}else{$ClientLoading}
        $taskArgs="--ip 127.0.0.1 --port $taskPeer --local-port $taskLocal --loading-frames $taskLoading --rounds $Rounds --frames 10000 --out ${taskRole}_record.txt"
        if($taskRole -eq 'host'){$taskArgs+=' --host'}
        if($taskRole -eq 'host' -and $HostStall){$taskArgs+=" --stall-after $HostStall --stall-ms 10000"}
        $taskProcesses += Start-Process "$taskRoot\build\bin\harness.exe" -WindowStyle Hidden -PassThru -WorkingDirectory $taskOut -ArgumentList $taskArgs -RedirectStandardOutput "$taskOut\$taskRole.log" -RedirectStandardError "$taskOut\$taskRole.err"
        # Windows PowerShellで終了後にExitCodeがnullになるのを防ぐためハンドルを保持する。
        $null = $taskProcesses[-1].Handle
        if($taskRole -eq 'host'){Start-Sleep -Milliseconds 500}
    }
    $taskProcesses.Id | ConvertTo-Json | Set-Content "$taskOut\pids.json"
    $taskDeadline=[DateTime]::UtcNow.AddSeconds(140)
    while(@($taskProcesses | Where-Object {!$_.HasExited}).Count -gt 0) {
        if([DateTime]::UtcNow -gt $taskDeadline){throw 'pair timeout'}
        Start-Sleep -Milliseconds 100
    }
    foreach($taskProcess in $taskProcesses) { $taskProcess.WaitForExit() }
    if($HostStall) {
        if(@($taskProcesses | Where-Object {$_.ExitCode -eq 0}).Count){throw '停止試験で異常終了を検出しなかった'}
        $taskEvidence=Get-Content "$taskOut\client.log" | Where-Object {$_ -match 'WAIT reason=remote input .*elapsedUs=(\d+) WT=(\d+)->(\d+) result=3'} | Select-Object -Last 1
        if(!$taskEvidence -or $taskEvidence -notmatch 'elapsedUs=(\d+) WT=(\d+)->(\d+) result=3'){throw '入力待機の期限到達記録がない'}
        if([int64]$Matches[1] -lt 3000000 -or [int64]$Matches[1] -gt 3500000 -or $Matches[2] -ne $Matches[3]){throw '期限/ゲーム停止の条件を満たさない'}
        @{passed=$true;scenario='peer_game_stall';evidence=$taskEvidence} | ConvertTo-Json | Set-Content "$taskOut\result.json"
        Write-Output $taskEvidence
    } else {
        foreach($taskProcess in $taskProcesses) { if($taskProcess.ExitCode -ne 0) { throw "harness exit=$($taskProcess.ExitCode)" } }
        & python (Join-Path $PSScriptRoot 'verify_bounded_pair.py') $taskOut
        if($LASTEXITCODE -ne 0){throw '入力/ゲーム状態の照合に失敗'}
    }
    Write-Output "Completed: $taskOut"
} finally {
    foreach($taskProcess in $taskProcesses) { if(!$taskProcess.HasExited){Stop-Process -Id $taskProcess.Id -Force} }
}
