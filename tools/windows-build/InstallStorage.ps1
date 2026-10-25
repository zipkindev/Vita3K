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
function Assert-ProtectionSeed($Seed){
 foreach($key in @('labels','sources','assets')){
  if($key -notin $Seed.PSObject.Properties.Name -or $Seed.$key -isnot [array]){throw "Protection seed requires an array: $key"}
  foreach($value in $Seed.$key){if($value -isnot [string] -or !$value){throw "Protection seed contains an invalid $key entry"}}
 }
 foreach($source in $Seed.sources){if($source -cnotmatch '^[0-9a-f]{40}$'){throw 'Protection seed contains an invalid source commit'}}
}
function Set-StorageDirectory([string]$Path,[bool]$RunnerRead){
 $current=[IO.Path]::GetFullPath($Path)
 while($current){
  if(Test-Path -LiteralPath $current){
   if((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw "Refusing installation through a reparse point: $current"}
  }
  $current=[IO.Path]::GetDirectoryName($current)
 }
 New-Item -ItemType Directory -Path $Path -Force | Out-Null
 if(Get-ChildItem -LiteralPath $Path -Recurse -Force | Where-Object {$_.Attributes -band [IO.FileAttributes]::ReparsePoint}){throw "Refusing installation tree with reparse points: $Path"}
 # Replace the DACL, rather than preserving unrelated explicit grants on a
 # surviving directory. Existing children must inherit this restricted DACL.
 $acl=New-Object Security.AccessControl.DirectorySecurity
 $acl.SetAccessRuleProtection($true,$false)
 $admin=New-Object Security.Principal.SecurityIdentifier('S-1-5-32-544')
 $acl.SetOwner($admin)
 foreach($sid in @('S-1-5-18','S-1-5-32-544')){
  $identity=New-Object Security.Principal.SecurityIdentifier($sid)
  $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($identity,'FullControl','ContainerInherit,ObjectInherit','None','Allow')))
 }
 if($RunnerRead){
  $identity=New-Object Security.Principal.SecurityIdentifier('S-1-5-20')
  $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($identity,'ReadAndExecute','ContainerInherit,ObjectInherit','None','Allow')))
 }
 Set-Acl -LiteralPath $Path -AclObject $acl
 foreach($child in Get-ChildItem -LiteralPath $Path -Force){
  & icacls.exe $child.FullName /setowner '*S-1-5-32-544' /T /Q | Out-Null
  if($LASTEXITCODE){throw "Owner setup failed: $($child.FullName)"}
  & icacls.exe $child.FullName /reset /T /Q | Out-Null
  if($LASTEXITCODE){throw "Child ACL reset failed: $($child.FullName)"}
 }
}
function Suspend-StorageTasks {
 $existingTasks=@(foreach($taskName in @('Vita3K-Restore-TrueNAS-Archive','Vita3K-Archive-Retention')){
  Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
 })
 foreach($task in $existingTasks){Disable-ScheduledTask -TaskName $task.TaskName | Out-Null}
 if($existingTasks.Count){
  $scheduler=New-Object -ComObject 'Schedule.Service'
  $scheduler.Connect()
  foreach($task in $existingTasks){
   if($scheduler.GetFolder('\').GetTask($task.TaskName).GetInstances(0).Count){throw "Task still has a running instance: $($task.TaskName)"}
  }
 }
}
if($ArchivePath -notmatch '^\\\\[^\\]+\\[^\\]+'){throw 'ArchivePath must be an SMB UNC path'}
foreach($path in @($python,"$runner\.runner",$ProtectionSeedPath)){
 if(!(Test-Path -LiteralPath $path -PathType Leaf)){throw "Required recovery input missing: $path"}
}
foreach($name in @('MaintainStorage.ps1','RestoreArchive.ps1','RetainArchives.py')){
 if(!(Test-Path -LiteralPath "$PSScriptRoot\$name")){throw "Missing companion script: $name"}
}
$seed=Get-Content -LiteralPath $ProtectionSeedPath -Raw | ConvertFrom-Json
Assert-ProtectionSeed $seed
# Validate surviving state before touching services, credentials or scripts.
$statePath="$retention\protected-builds.json"
$previous=if(Test-Path $statePath){Get-Content $statePath -Raw | ConvertFrom-Json}else{$null}
if($null -ne $previous){Assert-ProtectionSeed $previous}
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
 # Disable existing triggers before replacing files/state. On failure, leave them
 # disabled until validation succeeds on a later installation attempt.
 Suspend-StorageTasks
 foreach($dir in @($maintenance,$storage,$retention)){
  Set-StorageDirectory $dir ($dir -eq $maintenance)
 }
 Copy-Item "$PSScriptRoot\MaintainStorage.ps1" "$maintenance\MaintainStorage.ps1" -Force
 Copy-Item "$PSScriptRoot\RestoreArchive.ps1" "$storage\RestoreArchive.ps1" -Force
 Copy-Item "$PSScriptRoot\RetainArchives.py" "$retention\RetainArchives.py" -Force
 Add-Type -AssemblyName System.Security
 $bytes=[Text.Encoding]::UTF8.GetBytes($SmbCredential.GetNetworkCredential().Password)
 try {
  $protected=[Security.Cryptography.ProtectedData]::Protect($bytes,[Text.Encoding]::UTF8.GetBytes('Vita3KRunnerStorage-v1'),[Security.Cryptography.DataProtectionScope]::LocalMachine)
  [ordered]@{Username=$SmbCredential.UserName;ProtectedPassword=[Convert]::ToBase64String($protected);RemotePath=$ArchivePath;LocalPath='Y:'} | ConvertTo-Json | Set-Content "$storage\credential.json" -Encoding UTF8
 } finally {[Array]::Clear($bytes,0,$bytes.Length)}
 # Merge with surviving state; never discard earlier release/draft preservation rules.
 $merged=[ordered]@{}
 foreach($key in @('labels','sources','assets')){$merged[$key]=@(@($seed.$key)+@($previous.$key) | Where-Object {$_} | Sort-Object -Unique)}
 $merged | ConvertTo-Json | Set-Content $statePath -Encoding UTF8
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
