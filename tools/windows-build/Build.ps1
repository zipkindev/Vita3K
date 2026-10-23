param(
 [ValidatePattern('^(?:[0-9a-f]{40})?$')][string]$SourceCommit,
 [ValidatePattern('^(?:[A-Za-z0-9][A-Za-z0-9._-]{0,63})?$')][string]$BuildLabel,
 [ValidateRange(1,16)][int]$Parallel=6,
 [switch]$PreflightOnly
)
$ErrorActionPreference='Stop'
$tools=$env:VITA3K_TOOLCHAIN_ROOT
if(!$tools){throw 'Set VITA3K_TOOLCHAIN_ROOT to the installed Windows toolchain directory'}
$vs="$tools\VS2022"
$qt="$tools\Qt\6.11.0\msvc2022_64"
$python="$tools\Python313\python.exe"
$cmake="$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja="$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
foreach($file in @($cmake,$ninja,$python,"$qt\bin\qmake.exe","$tools\Git\cmd\git.exe")){
 if(!(Test-Path $file)){throw "Required tool missing: $file"}
}
$env:VSCMD_SKIP_SENDTELEMETRY='1'
$lines=& $env:ComSpec /d /s /c "`"`"$vs\Common7\Tools\VsDevCmd.bat`" -arch=x64 -host_arch=x64 >nul && set`""
if($LASTEXITCODE){throw 'MSVC environment initialization failed'}
foreach($line in $lines){if($line -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
$env:PATH="$tools\Git\cmd;$tools\Python313;$qt\bin;$env:PATH"
$env:VCPKG_ROOT="$tools\vcpkg"
$env:VCPKG_DEFAULT_BINARY_CACHE='C:\b\cache\vcpkg'
$env:VCPKG_MAX_CONCURRENCY=[string]$Parallel
$env:Qt6_ROOT=$qt
$env:CMAKE_POLICY_VERSION_MINIMUM='3.5'
$env:GIT_CONFIG_COUNT='1'
$env:GIT_CONFIG_KEY_0='safe.directory'
$env:GIT_CONFIG_VALUE_0='*'
New-Item -ItemType Directory -Force $env:VCPKG_DEFAULT_BINARY_CACHE | Out-Null
if($PreflightOnly){
 $dir=Join-Path $env:RUNNER_TEMP 'compiler-probe'
 New-Item -ItemType Directory -Force $dir | Out-Null
 Set-Location $dir
 '#include <windows.h>'+"`n"+'int main(){return GetCurrentProcessId()==0;}' | Set-Content probe.cpp -Encoding ascii
 $ErrorActionPreference='Continue'
 & cl.exe /nologo /O2 /Z7 probe.cpp /Fe:probe.exe /link /DEBUG:FULL /PDB:probe.pdb
 if($LASTEXITCODE){throw 'Compiler/linker probe failed'}
 & .\probe.exe
 if($LASTEXITCODE){throw 'Native executable probe failed'}
 & $python "$PSScriptRoot\verify_pe_pdb.py" probe.exe probe.pdb
 if($LASTEXITCODE){throw 'Matching-symbol probe failed'}
 & "$qt\bin\qmake.exe" -query QT_VERSION
 if($LASTEXITCODE){throw 'Qt probe failed'}
 & git --version
 & $python --version
 'Windows runner readiness checks passed; no emulator build was requested.'
 exit 0
}
if(!$SourceCommit -or !$BuildLabel){throw 'SourceCommit and BuildLabel are required for a release package'}
$source=Join-Path $env:GITHUB_WORKSPACE 'source'
& git -c core.autocrlf=false clone --no-checkout "$($env:GITHUB_SERVER_URL)/$($env:GITHUB_REPOSITORY).git" $source
if($LASTEXITCODE){throw 'Public source clone failed'}
& git -C $source checkout --detach $SourceCommit
if($LASTEXITCODE){throw 'Requested source commit unavailable'}
& git -C $source submodule update --init --recursive --jobs $Parallel
if($LASTEXITCODE){throw 'Pinned submodule checkout failed'}
& $python "$PSScriptRoot\ApplyDependencyPatches.py" --source $source
if($LASTEXITCODE){throw 'Source-pinned dependency patch failed'}
if((Get-Content "$source\vita3k\ngs\src\modules\atrac9.cpp" -Raw) -notmatch 'superframe_bytes_left'){throw 'Selected source lacks the corrected bounded NGS audio caller'}
foreach($name in @('diagnostic-identity.patch','diagnostic-build-support.patch','atrac9-input-span.patch')){
 & git -C $source apply "$PSScriptRoot\patches\$name"
 if($LASTEXITCODE){throw "Source support patch failed: $name"}
}
& git -C "$source\external\LibAtrac9" apply "$PSScriptRoot\patches\libatrac9-bounded.patch"
if($LASTEXITCODE){throw 'Missing bounded decoder support patch failed'}
# These support patches target the separately sourced Plus baseline; unrelated sources fail closed.
$version="$source\vita3k\CMakeLists.txt"
$text=[IO.File]::ReadAllText($version)
$text=[regex]::Replace($text,'set\(VITA3K_APP_VERSION "[^"\r\n]+"\)',('set(VITA3K_APP_VERSION "'+$BuildLabel+'")'))
[IO.File]::WriteAllText($version,$text,(New-Object Text.UTF8Encoding($false)))
$header="$source\vita3k\util\include\util\fork_build.h"
$text=[IO.File]::ReadAllText($header).Replace('v1.3.3-win11diag.1',$BuildLabel).Replace('a86b5f93db43e4c4640ad1527e2a4695aad0f35b',$SourceCommit)
$sequence=if($BuildLabel -match '(\d+)$'){$matches[1]}else{'1'}
$text=$text.Replace('#define FORK_BUILD_SEQ 1',('#define FORK_BUILD_SEQ '+$sequence))
[IO.File]::WriteAllText($header,$text,(New-Object Text.UTF8Encoding($false)))
& git -C $source add -N vita3k/util/include/util/fork_build.h
$build=Join-Path $env:GITHUB_WORKSPACE 'native-build'
$evidence=Join-Path $env:GITHUB_WORKSPACE 'build-evidence'
New-Item -ItemType Directory -Force $evidence | Out-Null
Set-Location $source
$ErrorActionPreference='Continue'
& $cmake -S $source -B $build -G 'Ninja Multi-Config' "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_TOOLCHAIN_FILE=$tools/vcpkg/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static-md -DCMAKE_CONFIGURATION_TYPES=RelWithDebInfo -DCMAKE_AUTOGEN_PARALLEL=4 -DUSE_LTO=ALWAYS -DTRACY_ENABLE_ON_CORE_COMPONENTS=OFF '-DCMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO=/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO' 2>&1 | Tee-Object "$evidence\configure.log"
if($LASTEXITCODE){throw 'Configure failed'}
& $cmake --build $build --config RelWithDebInfo --parallel $Parallel 2>&1 | Tee-Object "$evidence\build.log"
if($LASTEXITCODE){throw 'Build failed'}
$env:PATH="$build\bin\RelWithDebInfo;$env:PATH"
& (Join-Path (Split-Path $cmake) 'ctest.exe') --test-dir $build -C RelWithDebInfo --output-on-failure 2>&1 | Tee-Object "$evidence\tests.log"
if($LASTEXITCODE){throw 'Tests failed'}
& git diff --binary | Set-Content "$evidence\source.patch"
& git -C external/LibAtrac9 diff --binary | Set-Content "$evidence\libatrac9.patch"
& git -C external/dynarmic diff --binary | Set-Content "$evidence\dynarmic.patch"
if(Test-Path "$source/tools/dependency-patches/manifest.json"){
 Copy-Item "$source/tools/dependency-patches/manifest.json" "$evidence/dependency-patches.json"
}
& git submodule status --recursive | Set-Content "$evidence\submodules.txt"
& cl.exe /Bv 2>&1 | Out-File "$evidence\compiler.txt"
& $python "$PSScriptRoot\Collect.py" --source $source --build $build --evidence $evidence --label $BuildLabel
if($LASTEXITCODE){throw 'Package or matching-PDB verification failed'}
