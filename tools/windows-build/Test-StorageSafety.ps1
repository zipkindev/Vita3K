#Requires -RunAsAdministrator
param([string]$SourceDirectory=$PSScriptRoot)
$ErrorActionPreference='Stop'
# Load only function definitions. Never execute the installer or storage hook.
foreach($name in @('InstallStorage.ps1','MaintainStorage.ps1','RestoreArchive.ps1')){
 $tokens=$null;$errors=$null
 $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $SourceDirectory $name),[ref]$tokens,[ref]$errors)
 if($errors.Count){throw ($errors | Out-String)}
 foreach($definition in $ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst]},$true)){
  . ([scriptblock]::Create($definition.Extent.Text))
 }
}
function Assert-Throws([scriptblock]$Action,[string]$Message){
 $thrown=$false
 try {& $Action} catch {$thrown=$true}
 if(!$thrown){throw $Message}
}
$fixture=Join-Path $env:TEMP ('vita3k-storage-safety-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
try {
 Assert-ProtectionSeed ('{"labels":[],"sources":[],"assets":[]}' | ConvertFrom-Json)
 foreach($json in @('{}','{"labels":"released","sources":[],"assets":[]}','{"labels":[],"sources":["bad"],"assets":[]}')){
  Assert-Throws {Assert-ProtectionSeed ($json | ConvertFrom-Json)} 'Malformed seed was accepted'
 }
 $target=Join-Path $fixture 'outside'
 New-Item -ItemType Directory -Path $target | Out-Null
 $junction=Join-Path $fixture 'junction'
 New-Item -ItemType Junction -Path $junction -Target $target | Out-Null
 try {
  Assert-Throws {Assert-PlainParents (Join-Path $junction 'not-created\child')} 'Archive path followed an ancestor junction'
  Assert-Throws {Set-StorageDirectory (Join-Path $junction 'not-created') $false} 'Installer followed an ancestor junction'
  if(Test-Path (Join-Path $target 'not-created')){throw 'Junction target was modified'}
 } finally {[IO.Directory]::Delete($junction)}
 $private=Join-Path $fixture 'private'
 New-Item -ItemType Directory -Path $private | Out-Null
 $file=Join-Path $private 'dummy.txt'
 'No secrets; ACL regression fixture' | Set-Content $file
 & icacls.exe $file /grant '*S-1-1-0:F' | Out-Null
 if($LASTEXITCODE){throw 'Could not prepare permissive ACL fixture'}
 Set-StorageDirectory $private $false
 foreach($path in @($private,$file)){
  foreach($rule in (Get-Acl -LiteralPath $path).Access){
   $sid=$rule.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value
   if($sid -notin @('S-1-5-18','S-1-5-32-544')){throw "Unrelated access survived ACL hardening: $sid"}
  }
 }
 Set-StorageDirectory $private $true
 $networkRules=@((Get-Acl $file).Access | Where-Object {$_.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value -eq 'S-1-5-20'})
 if(!$networkRules.Count -or $networkRules[0].FileSystemRights -band [Security.AccessControl.FileSystemRights]::Write){throw 'Network Service needs read/execute access without write access'}
 Write-Output 'Native seed, junction, and ACL regression tests passed.'
} finally {Remove-Item -LiteralPath $fixture -Recurse -Force}

# Exercise task suspension using scoped mocks; no live tasks are changed.
& {
 $script:disabled=@()
 $script:running=0
 function Get-ScheduledTask($TaskName){[pscustomobject]@{TaskName=$TaskName}}
 function Disable-ScheduledTask($TaskName){$script:disabled+=$TaskName}
 $registered=[pscustomobject]@{}
 $registered | Add-Member ScriptMethod GetInstances {param($flags) [pscustomobject]@{Count=$script:running}}
 $folder=[pscustomobject]@{Registered=$registered}
 $folder | Add-Member ScriptMethod GetTask {param($name) if($script:disabled.Count -ne 2){throw 'Tasks were inspected before both triggers were disabled'};$this.Registered}
 $scheduler=[pscustomobject]@{Folder=$folder}
 $scheduler | Add-Member ScriptMethod Connect {}
 $scheduler | Add-Member ScriptMethod GetFolder {param($path) $this.Folder}
 $script:mockScheduler=$scheduler
 function New-Object($ComObject){if($ComObject -ne 'Schedule.Service'){throw 'Unexpected COM request'};$script:mockScheduler}
 Suspend-StorageTasks
 if($script:disabled.Count -ne 2){throw 'Existing triggers were not both disabled'}
 $script:disabled=@();$script:running=1
 Assert-Throws {Suspend-StorageTasks} 'Installer accepted a still-running task'
 if($script:disabled.Count -ne 2){throw 'Failed suspension re-enabled unsafe triggers'}
}
Write-Output 'Mocked task suspension and active-instance regression tests passed.'
& {
 $config=[pscustomobject]@{LocalPath='Y:';RemotePath='\\fixture\archive'}
 $script:mapping=[pscustomobject]@{RemotePath=$config.RemotePath}
 $script:available=$true;$script:mappingActions=@()
 function Get-SmbGlobalMapping($LocalPath){$script:mapping}
 function Test-Path($LiteralPath,$PathType){$script:available}
 function Remove-SmbGlobalMapping($LocalPath,[switch]$Force){$script:mappingActions+='remove';$script:mapping=$null}
 function New-SmbGlobalMapping($LocalPath,$RemotePath,$Credential,$Persistent,$RequireIntegrity,$FullAccess){$script:mappingActions+='create';$script:available=$true}
 Connect-ArchiveMapping $config $null
 if($script:mappingActions.Count){throw 'A healthy mapping was unnecessarily replaced'}
 $script:available=$false
 Connect-ArchiveMapping $config $null
 if(($script:mappingActions -join ',') -ne 'remove,create'){throw 'Stale mapping was not replaced before reconnecting'}
 $script:mappingActions=@();$script:mapping=[pscustomobject]@{RemotePath='\\fixture\unrelated'}
 Assert-Throws {Connect-ArchiveMapping $config $null} 'An unrelated mapping was accepted'
 if($script:mappingActions.Count){throw 'An unrelated mapping was modified'}
}
Write-Output 'Mocked healthy, stale, and unrelated SMB mapping regressions passed.'
