param([string]$ConfigPath=(Join-Path $PSScriptRoot 'credential.json'))
# Install in an administrator/SYSTEM-only directory and run as SYSTEM at startup.
# The protected configuration is provisioned separately over pinned SSH stdin.
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
Add-Type -AssemblyName System.Security
$config=Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json
$entropy=[Text.Encoding]::UTF8.GetBytes('Vita3KRunnerStorage-v1')
$bytes=[Security.Cryptography.ProtectedData]::Unprotect(
 [Convert]::FromBase64String($config.ProtectedPassword),$entropy,
 [Security.Cryptography.DataProtectionScope]::LocalMachine)
try {
 $secure=New-Object Security.SecureString
 foreach($character in [Text.Encoding]::UTF8.GetString($bytes).ToCharArray()){$secure.AppendChar($character)}
 $secure.MakeReadOnly()
 $credential=New-Object Management.Automation.PSCredential($config.Username,$secure)
} finally {[Array]::Clear($bytes,0,$bytes.Length)}
$existing=Get-SmbGlobalMapping -LocalPath $config.LocalPath -ErrorAction SilentlyContinue
if($existing -and $existing.RemotePath -ne $config.RemotePath){throw 'Archive drive is mapped to another destination; refusing replacement'}
for($attempt=1;$attempt -le 60;$attempt++){
 try {
  if(!(Get-SmbGlobalMapping -LocalPath $config.LocalPath -ErrorAction SilentlyContinue)){
   New-SmbGlobalMapping -LocalPath $config.LocalPath -RemotePath $config.RemotePath -Credential $credential -Persistent $true -RequireIntegrity $true -FullAccess @('NT AUTHORITY\SYSTEM','NT AUTHORITY\NETWORK SERVICE','BUILTIN\Administrators') | Out-Null
  }
  if(!(Test-Path -LiteralPath ($config.LocalPath+'\') -PathType Container)){throw 'Archive path is not yet available'}
  [ordered]@{Utc=[DateTime]::UtcNow.ToString('o');BootUtc=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o');Identity=[Security.Principal.WindowsIdentity]::GetCurrent().Name;LocalPath=$config.LocalPath;RemotePath=$config.RemotePath;Attempt=$attempt;Connected=$true} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'restore-result.json')
  exit 0
 } catch {
  if($attempt -eq 60){throw 'TrueNAS archive could not be restored within the startup retry window'}
  Start-Sleep -Seconds 5
 }
}
