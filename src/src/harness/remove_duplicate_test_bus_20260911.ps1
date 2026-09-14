#Requires -RunAsAdministrator
# 2026-09-11の依存導入で増えたエラー項目だけを削除する。一括削除やドライバのアンインストールはしない。
$ErrorActionPreference='Stop'
$taskDuplicateId='ROOT\SYSTEM\0004'
$taskOriginal=Get-PnpDevice -InstanceId 'ROOT\SYSTEM\0001'
if($taskOriginal.Status -ne 'OK'){throw '既存バスの状態が確認時と異なるため停止'}
$taskDuplicate=Get-PnpDevice -InstanceId $taskDuplicateId -ErrorAction SilentlyContinue
if(!$taskDuplicate){Write-Output '重複項目は既に削除済み';exit 0}
$taskInstalled=(Get-PnpDeviceProperty -InstanceId $taskDuplicateId -KeyName 'DEVPKEY_Device_FirstInstallDate').Data
$taskProblem=(Get-PnpDeviceProperty -InstanceId $taskDuplicateId -KeyName 'DEVPKEY_Device_ProblemCode').Data
if($taskDuplicate.FriendlyName -ne 'Nefarius Virtual Gamepad Emulation Bus' -or $taskProblem -ne 31 -or
   $taskInstalled.ToString('yyyyMMddHHmm') -ne '202609111624'){
    throw '対象の識別・状態・作成時刻が確認時と異なるため停止'
}
& pnputil.exe /remove-device $taskDuplicateId
if($LASTEXITCODE -ne 0){throw '重複項目を削除できなかった'}
if((Get-PnpDevice -InstanceId 'ROOT\SYSTEM\0001').Status -ne 'OK'){throw '既存バスの状態を確認してください'}
