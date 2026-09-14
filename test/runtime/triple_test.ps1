param([switch]$Check)
$ErrorActionPreference = 'Stop'
# bat の --check と PowerShell の -Check の両方を受け付ける。
if ($args -contains '--check') { $Check = $true }
try {
    $taskRuntime = $PSScriptRoot
    $taskBuild = [IO.Path]::GetFullPath((Join-Path $taskRuntime '../../build/bin'))
    $taskPort = 7500
    $taskFiles = @('CCCaster_B.exe', 'libcccaster_hook.dll')
    $taskDirs = @(1..3 | ForEach-Object { Join-Path $taskRuntime "MBAACC_$_/cccaster_B" })
    $taskPlans = @(
        '--headless --host --port 7500 --sim-delay 50,90 --sim-loss 20'
        '--headless --ip 127.0.0.1 --port 7500 --sim-delay 50,90 --sim-loss 20'
        '--spectate --ip 127.0.0.1 --port 7500'
    )
    Write-Host '============================================'
    Write-Host '  CCCaster B 対戦2窓＋観戦1窓テスト'
    Write-Host '  対戦: 遅延50～90ms / 損失20%'
    Write-Host '============================================'
    foreach ($taskFile in $taskFiles) {
        if (!(Test-Path -LiteralPath (Join-Path $taskBuild $taskFile) -PathType Leaf)) {
            throw "ビルド成果物がありません: $taskBuild/$taskFile"
        }
    }
    foreach ($taskDir in $taskDirs) {
        if (!(Test-Path -LiteralPath $taskDir -PathType Container)) { throw "配置先がありません: $taskDir" }
        if (!(Test-Path -LiteralPath (Join-Path $taskDir '../MBAA.exe') -PathType Leaf)) { throw "ゲームがありません: $taskDir/../MBAA.exe" }
    }
    # 対象3コピーだけを確認し、既存プロセスは終了させない。
    $taskGameRoots = @($taskDirs | ForEach-Object { (Split-Path -Parent $_) + '\' })
    $taskBusy = @(Get-CimInstance Win32_Process | Where-Object {
        $taskPath = $_.ExecutablePath
        $taskPath -and @($taskGameRoots | Where-Object {
            $taskPath.StartsWith($_, [StringComparison]::OrdinalIgnoreCase)
        }).Count
    })
    if ($taskBusy.Count) { throw 'テスト用ゲームまたはランチャーを閉じてから再実行してください。' }
    if ((Get-NetUDPEndpoint -LocalPort $taskPort -ErrorAction SilentlyContinue) -or
        (Get-NetTCPConnection -LocalPort $taskPort -State Listen -ErrorAction SilentlyContinue)) {
        throw 'ポート7500が使用中です。'
    }
    if ($Check) {
        Write-Host '[確認OK] 必要ファイル・対象プロセス・ポートを確認しました。'
        for ($taskIndex = 0; $taskIndex -lt 3; $taskIndex++) {
            Write-Host ("MBAACC_{0}: {1}" -f ($taskIndex + 1), $taskPlans[$taskIndex])
        }
        exit 0
    }
    $taskOut = Join-Path $taskRuntime ('../logs/triple_test_' + (Get-Date -Format 'yyyyMMdd_HHmmss_fff'))
    New-Item -ItemType Directory -Path $taskOut | Out-Null
    for ($taskIndex = 0; $taskIndex -lt 3; $taskIndex++) {
        $taskDest = Join-Path $taskOut ('before_' + ($taskIndex + 1))
        New-Item -ItemType Directory -Path $taskDest | Out-Null
        Get-ChildItem -LiteralPath $taskDirs[$taskIndex] -File | Where-Object {
            $_.Extension -eq '.log' -or $_.Name -like '*log*.txt'
        } | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $taskDest }
    }
    Write-Host "既存ログの保存先: $taskOut"
    Write-Host '[準備] 最新EXE・DLLを3組へ反映します。'
    foreach ($taskDir in $taskDirs) {
        foreach ($taskFile in $taskFiles) {
            Copy-Item -LiteralPath (Join-Path $taskBuild $taskFile) -Destination (Join-Path $taskDir $taskFile) -Force
        }
    }
    Write-Host '[1/3] ホストを起動します。'
    $taskHostProcess = Start-Process -FilePath (Join-Path $taskDirs[0] 'CCCaster_B.exe') -WorkingDirectory $taskDirs[0] -ArgumentList $taskPlans[0] -PassThru
    Start-Sleep -Seconds 3
    if ($taskHostProcess.HasExited) { throw 'ホストが終了しました。ホスト側の表示を確認してください。' }
    Write-Host '[2/3] 対戦相手を起動します。'
    Start-Process -FilePath (Join-Path $taskDirs[1] 'CCCaster_B.exe') -WorkingDirectory $taskDirs[1] -ArgumentList $taskPlans[1]
    Write-Host '観戦のTCP待受を最大30秒待ちます。'
    $taskUntil = [DateTime]::UtcNow.AddSeconds(30)
    do {
        if ($taskHostProcess.HasExited) { throw 'ホストが終了しました。ホスト側の表示を確認してください。' }
        $taskListening = Get-NetTCPConnection -LocalPort $taskPort -State Listen -ErrorAction SilentlyContinue
        if ($taskListening) { break }
        Start-Sleep -Milliseconds 500
    } while ([DateTime]::UtcNow -lt $taskUntil)
    if (!$taskListening) { throw '観戦の待受が始まりませんでした。ホスト側の表示を確認してください。' }
    Write-Host '[3/3] 観戦を起動します。'
    Start-Process -FilePath (Join-Path $taskDirs[2] 'CCCaster_B.exe') -WorkingDirectory $taskDirs[2] -ArgumentList $taskPlans[2]
    Write-Host '3つの起動コマンドを実行しました。各窓は手動で終了してください。'
    Write-Host '対戦: MBAACC_1 と MBAACC_2 / 観戦: MBAACC_3'
    Write-Host '現在のログ: 各 MBAACC_*\cccaster_B\cccaster_hook_log.txt'
    Write-Host 'このバッチは同期一致の自動判定は行いません。'
    exit 0
} catch {
    Write-Host "[中止] $_" -ForegroundColor Red
    Write-Host '起動済みの窓がある場合は手動で閉じてください。'
    exit 1
}
