#Requires -RunAsAdministrator
param(
 [Parameter(Mandatory=$true)][string]$ArchivePath,
 [Parameter(Mandatory=$true)][pscredential]$SmbCredential,
 [Parameter(Mandatory=$true)][string]$ProtectionSeedPath
)
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$runner='C:\b\runner'
$maintenance='C:\Vita3K-Build\maintenance'
$storage='C:\ProgramData\Vita3KRunnerStorage'
$retention='C:\ProgramData\Vita3KArchiveRetention'
$python='C:\Vita3K-Build\tools\Python313\python.exe'
$hook='ACTIONS_RUNNER_HOOK_JOB_COMPLETED='+$maintenance+'\MaintainStorage.ps1'
if($ArchivePath -notmatch '^\\\\[^\\]+\\[^\\]+'){throw 'ArchivePath must be an SMB UNC path'}
foreach($path in @($python,"$runner\.runner",$ProtectionSeedPath)){
 if(!(Test-Path -LiteralPath $path -PathType Leaf)){throw "Required recovery input missing: $path"}
}
foreach($name in @('MaintainStorage.ps1','RestoreArchive.ps1','RetainArchives.py')){
 if(!(Test-Path -LiteralPath "$PSScriptRoot\$name")){throw "Missing companion script: $name"}
}
$seed=Get-Content -LiteralPath $ProtectionSeedPath -Raw | ConvertFrom-Json
foreach($key in @('labels','sources','assets')){if($key -notin $seed.PSObject.Properties.Name){throw "Protection seed lacks $key"}}
$services=@(Get-CimInstance Win32_Service | Where-Object {$_.Name -like 'actions.runner*' -and $_.PathName -like "*$runner\bin\RunnerService.exe*"})
if($services.Count -ne 1 -or $services[0].StartName -ne 'NT AUTHORITY\NETWORK SERVICE'){throw 'Expected one registered runner service using Network Service'}
if(Get-Process Runner.Worker,cl,link,ninja,cmake -ErrorAction SilentlyContinue){throw 'Runner busy; installation deferred'}
foreach($taskName in @('Vita3K-Restore-TrueNAS-Archive','Vita3K-Archive-Retention')){
 if((Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue).State -eq 'Running'){throw "Task is running: $taskName"}
}
$existingHook=@(Get-Content "$runner\.env" -ErrorAction SilentlyContinue | Where-Object {$_ -match '^ACTIONS_RUNNER_HOOK_JOB_COMPLETED='})
if($existingHook.Count -gt 1 -or ($existingHook.Count -eq 1 -and $existingHook[0] -ne $hook)){throw 'Preserving an existing different job-completed hook'}
$mapping=Get-SmbGlobalMapping -LocalPath 'Y:' -ErrorAction SilentlyContinue
if($mapping -and $mapping.RemotePath -ne $ArchivePath){throw 'Y: already maps another destination'}
$wasRunning=$services[0].State -eq 'Running'
Stop-Service -Name $services[0].Name
try {
 if(Get-Process Runner.Worker,cl,link,ninja,cmake -ErrorAction SilentlyContinue){throw 'Build activity appeared; installation deferred'}
 foreach($dir in @($maintenance,$storage,$retention)){
  New-Item -ItemType Directory -Path $dir -Force | Out-Null
  $grants=@('*S-1-5-18:(OI)(CI)F','*S-1-5-32-544:(OI)(CI)F')
  if($dir -eq $maintenance){$grants+='*S-1-5-20:(OI)(CI)RX'}
  & icacls.exe $dir /inheritance:r /grant:r $grants | Out-Null
  if($LASTEXITCODE){throw "ACL setup failed: $dir"}
 }
 Copy-Item "$PSScriptRoot\MaintainStorage.ps1" "$maintenance\MaintainStorage.ps1" -Force
 Copy-Item "$PSScriptRoot\RestoreArchive.ps1" "$storage\RestoreArchive.ps1" -Force
 Copy-Item "$PSScriptRoot\RetainArchives.py" "$retention\RetainArchives.py" -Force
 Add-Type -AssemblyName System.Security
 $bytes=[Text.Encoding]::UTF8.GetBytes($SmbCredential.GetNetworkCredential().Password)
 try {
  $protected=[Security.Cryptography.ProtectedData]::Protect($bytes,[Text.Encoding]::UTF8.GetBytes('Vita3KRunnerStorage-v1'),[Security.Cryptography.DataProtectionScope]::LocalMachine)
  [ordered]@{Username=$SmbCredential.UserName;ProtectedPassword=[Convert]::ToBase64String($protected);RemotePath=$ArchivePath;LocalPath='Y:'} | ConvertTo-Json | Set-Content "$storage\credential.json"
 } finally {[Array]::Clear($bytes,0,$bytes.Length)}
 # Merge with surviving state; never discard earlier release/draft preservation rules.
 $statePath="$retention\protected-builds.json"
 $previous=if(Test-Path $statePath){Get-Content $statePath -Raw | ConvertFrom-Json}else{$null}
 $merged=[ordered]@{}
 foreach($key in @('labels','sources','assets')){$merged[$key]=@(@($seed.$key)+@($previous.$key) | Where-Object {$_} | Sort-Object -Unique)}
 $merged | ConvertTo-Json | Set-Content $statePath
 & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$storage\RestoreArchive.ps1"
 if($LASTEXITCODE){throw 'Archive restore validation failed'}
 & $python "$retention\RetainArchives.py" --state $retention
 if($LASTEXITCODE){throw 'Retention dry run failed; scheduled cleanup was not enabled'}
 $wrapper=@'
$ErrorActionPreference='Stop'
& 'C:\Vita3K-Build\tools\Python313\python.exe' 'C:\ProgramData\Vita3KArchiveRetention\RetainArchives.py' --state 'C:\ProgramData\Vita3KArchiveRetention' --days 30 --keep 5 --apply *> 'C:\ProgramData\Vita3KArchiveRetention\task.log'
exit $LASTEXITCODE
'@
 $wrapper | Set-Content "$retention\RunRetention.ps1"
 $principal=New-ScheduledTaskPrincipal -UserId 'NT AUTHORITY\SYSTEM' -LogonType ServiceAccount -RunLevel Highest
 $action=New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File $storage\RestoreArchive.ps1"
 $settings=New-ScheduledTaskSettingsSet -StartWhenAvailable -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit (New-TimeSpan -Minutes 10) -MultipleInstances IgnoreNew
 Register-ScheduledTask -TaskName 'Vita3K-Restore-TrueNAS-Archive' -Action $action -Trigger (New-ScheduledTaskTrigger -AtStartup) -Principal $principal -Settings $settings -Force | Out-Null
 $action=New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File $retention\RunRetention.ps1"
 $settings=New-ScheduledTaskSettingsSet -StartWhenAvailable -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 15) -ExecutionTimeLimit (New-TimeSpan -Minutes 30) -MultipleInstances IgnoreNew
 Register-ScheduledTask -TaskName 'Vita3K-Archive-Retention' -Action $action -Trigger (New-ScheduledTaskTrigger -Daily -At '03:30') -Principal $principal -Settings $settings -Force | Out-Null
 if(!$existingHook.Count){Add-Content "$runner\.env" -Value "`r`n$hook" -Encoding UTF8}
 Write-Output 'Storage hook and startup/retention tasks installed; retention dry run passed.'
} finally {
 if($wasRunning){Start-Service -Name $services[0].Name}
}
