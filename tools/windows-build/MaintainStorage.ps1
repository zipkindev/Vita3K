param(
 [string]$Workspace=$env:GITHUB_WORKSPACE,
 [string]$ArchiveRoot='Y:\',
 [switch]$InspectOnly
)
# Run as ACTIONS_RUNNER_HOOK_JOB_COMPLETED, after artifact upload.
# Archive finished packages, then discard only reproducible compiler output.
$ErrorActionPreference='Stop'
function Assert-PlainParents([string]$Path){
 $current=[IO.Path]::GetFullPath($Path)
 while($current){
  if(Test-Path -LiteralPath $current){
   if((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw "Refusing reparse point: $current"}
  }
  $current=[IO.Path]::GetDirectoryName($current)
 }
}
function Assert-PlainTree([string]$Path){
 Assert-PlainParents $Path
 if((Get-Item -LiteralPath $Path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw "Refusing reparse point: $Path"}
 if(Get-ChildItem -LiteralPath $Path -Recurse -Force | Where-Object {$_.Attributes -band [IO.FileAttributes]::ReparsePoint}){throw "Refusing tree with reparse points: $Path"}
}
function Assert-SameFile([string]$Left,[string]$Right){
 if(!(Test-Path -LiteralPath $Right -PathType Leaf)){throw "Missing preserved file: $Right"}
 if((Get-FileHash -LiteralPath $Left -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $Right -Algorithm SHA256).Hash){throw "Preserved file hash mismatch: $Right"}
}
function Copy-VerifiedTree([string]$From,[string]$To){
 Assert-PlainTree $From
 New-Item -ItemType Directory -Path $To -Force | Out-Null
 Assert-PlainTree $To
 & robocopy.exe $From $To /E /COPY:DAT /DCOPY:T /R:1 /W:2 /XJ /NFL /NDL /NJH /NJS /NP | Out-Null
 if($LASTEXITCODE -ge 8){throw "Archive copy failed: $From"}
 foreach($file in Get-ChildItem -LiteralPath $From -Recurse -File -Force){
  $relative=$file.FullName.Substring($From.TrimEnd('\').Length+1)
  Assert-SameFile $file.FullName (Join-Path $To $relative)
 }
}
$archiveLock=$null
try {
 $allowed=@('Vita3K','Vita3K-ZipZ','Vita3K-Go') | ForEach-Object {"C:\b\_work\$_\$_"}
 if(!$Workspace){throw 'Workspace is required'}
 $Workspace=[IO.Path]::GetFullPath($Workspace).TrimEnd('\')
 if($Workspace -notin $allowed){throw "Workspace outside dedicated runner allowlist: $Workspace"}
 Assert-PlainTree $Workspace
 # A completed hook still has its Runner.Worker process. Compiler activity must be absent.
 if(Get-Process cl,link,ninja,cmake -ErrorAction SilentlyContinue){throw 'Compiler still active; storage maintenance skipped'}
 $package=Join-Path $Workspace 'windows-package'
 $build=Join-Path $Workspace 'native-build'
 $manifestPath=Join-Path $package 'provenance.json'
 if(!(Test-Path -LiteralPath $manifestPath)){Write-Output 'No completed package; retaining workspace for diagnosis.';exit 0}
 $manifest=Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
 if($manifest.buildLabel -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$' -or [string]$manifest.runId -notmatch '^\d+$'){throw 'Invalid package identity'}
 if(!$manifest.files -or !$manifest.sourceCommit){throw 'Incomplete package provenance'}
 foreach($file in $manifest.files){
  $path=[IO.Path]::GetFullPath((Join-Path $package $file.path))
  if(!$path.StartsWith($package+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Manifest path outside package'}
  if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $file.sha256){throw "Package manifest mismatch: $($file.path)"}
 }
 if(Test-Path -LiteralPath $build){
  Assert-SameFile (Join-Path $build 'bin\RelWithDebInfo\Vita3K.exe') (Join-Path $package 'Vita3K.exe')
  Assert-SameFile (Join-Path $build 'bin\RelWithDebInfo\Vita3K.pdb') (Join-Path $package 'Vita3K.pdb')
 }
 $repo=Split-Path $Workspace -Leaf
 $destination=Join-Path $ArchiveRoot "$repo\$($manifest.buildLabel)-$($manifest.runId)"
 if($InspectOnly){
  [ordered]@{Workspace=$Workspace;Archive=$destination;PackageVerified=$true;WouldRemove=$build} | ConvertTo-Json
  exit 0
 }
 if(!(Test-Path -LiteralPath $ArchiveRoot -PathType Container)){throw 'Archive unavailable; retaining all local files'}
 Assert-PlainParents (Join-Path $ArchiveRoot '.maintenance.lock')
 $archiveLock=[IO.File]::Open((Join-Path $ArchiveRoot '.maintenance.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
 Assert-PlainParents $destination
 New-Item -ItemType Directory -Path $destination -Force | Out-Null
 Assert-PlainTree $destination
 $archivedManifest=Join-Path $destination 'windows-package\provenance.json'
 if(Test-Path -LiteralPath $archivedManifest){Assert-SameFile $manifestPath $archivedManifest}
 foreach($name in @('windows-package','windows-downloads','build-evidence')){
  $from=Join-Path $Workspace $name
  if(Test-Path -LiteralPath $from){Copy-VerifiedTree $from (Join-Path $destination $name)}
 }
 $receipt=[ordered]@{Utc=[DateTime]::UtcNow.ToString('o');Identity=[Security.Principal.WindowsIdentity]::GetCurrent().Name;Workspace=$Workspace;Archive=$destination;Source=$manifest.sourceCommit;Run=$manifest.runId;PackageFiles=$manifest.files.Count;Verified=$true}
 $receipt | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $destination 'archive-verified.json')
 if(Test-Path -LiteralPath $build){Remove-Item -LiteralPath $build -Recurse -Force}
 $receipt | ConvertTo-Json
 Write-Output ('C: free GiB after maintenance: '+[math]::Round((Get-PSDrive C).Free/1GB,2))
} catch {
 # An archive outage must not turn a successful build into a failed job.
 Write-Warning ('Storage maintenance incomplete: '+$_.Exception.Message)
 if($InspectOnly){exit 1}
} finally {
 if($archiveLock){$archiveLock.Dispose()}
}
exit 0
