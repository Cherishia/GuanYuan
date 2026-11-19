param(
    [string]$ZigDir,
    [string]$JdkDir,
    [string]$BuildToolsDir,
    [string]$AndroidJar,
    [string]$BuildDir = (Join-Path $env:TEMP 'PalmPower-build'),
    [switch]$WindowsOnly
)
$ErrorActionPreference = 'Stop'
$release = Split-Path $PSScriptRoot -Parent
New-Item -ItemType Directory -Force -Path $BuildDir,(Join-Path $release 'Windows'),(Join-Path $release 'Android') | Out-Null
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
function Run-Tool([string]$Executable,[string[]]$ToolArguments) {
    & $Executable @ToolArguments
    if ($LASTEXITCODE -ne 0) { throw ('Build tool failed: ' + $Executable) }
}
$compiler = if ($ZigDir) { Join-Path $ZigDir 'zig.exe' } else { (Get-Command zig -ErrorAction Stop).Source }
$compiler = (Resolve-Path -LiteralPath $compiler).Path
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $BuildDir 'zig-global-cache'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $BuildDir 'zig-local-cache'
New-Item -ItemType Directory -Path $env:ZIG_GLOBAL_CACHE_DIR,$env:ZIG_LOCAL_CACHE_DIR -Force | Out-Null
$exe = Join-Path $release 'Windows\GuanYuan.exe'
$resource = Join-Path $BuildDir 'power.res'
Push-Location (Join-Path $PSScriptRoot 'Native')
try {
    Run-Tool $compiler @('rc','/fo',$resource,'app.rc')
} finally { Pop-Location }
$sources = @(Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'Native') -Filter '*.cpp' | ForEach-Object FullName)
Run-Tool $compiler (@('c++','-std=c++17','-O2','-s','-fno-rtti','-Wno-unused-command-line-argument','-municode','-Wl,--subsystem,windows','-DUNICODE','-D_UNICODE','-D_WIN32_WINNT=0x0a00','-DWIN32_LEAN_AND_MEAN','-DNOMINMAX','-o',$exe)+$sources+@($resource,'-lws2_32','-lbcrypt','-lcrypt32','-ladvapi32','-lshell32','-liphlpapi','-lpowrprof','-lgdi32','-luser32','-lcomctl32','-luuid','-lwinhttp','-ldwmapi'))
# Check the linked PE rather than relying on a driver flag being honoured.
$peBytes = [System.IO.File]::ReadAllBytes($exe)
$peOffset = [BitConverter]::ToInt32($peBytes,60)
if ([BitConverter]::ToUInt16($peBytes,$peOffset+24+68) -ne 2) { throw 'The desktop executable must use the Windows GUI subsystem.' }
# Windows PowerShell 5.1 needs a BOM to display the Chinese messages correctly.
$firewall = Join-Path $release 'Windows\AllowNetwork.ps1'
[System.IO.File]::WriteAllText($firewall,[System.IO.File]::ReadAllText($firewall),[System.Text.UTF8Encoding]::new($true))
Write-Output ('Built ' + $exe)
if ($WindowsOnly) { return }
if (-not $JdkDir -or -not $BuildToolsDir -or -not (Test-Path -LiteralPath $AndroidJar)) { throw 'Provide -JdkDir, -BuildToolsDir, and -AndroidJar from an installed JDK 17 and Android SDK 35.' }
$android = Join-Path $PSScriptRoot 'Android'
$expectedName = ([string][char]0x5173)+([string][char]0x6e90)
[xml]$stringResources = [System.IO.File]::ReadAllText((Join-Path $android 'res\values\strings.xml'))
if ($stringResources.resources.string.InnerText -cne $expectedName) { throw 'The app_name resource must decode to the correct Chinese name.' }
$aapt2 = Join-Path $BuildToolsDir 'aapt2.exe'
$java = Join-Path $JdkDir 'bin\java.exe'
$classes = Join-Path $BuildDir 'classes'
$dex = Join-Path $BuildDir 'dex'
$generated = Join-Path $BuildDir 'generated'
New-Item -ItemType Directory -Force -Path $classes,$dex,$generated | Out-Null
# Remove only the previous build's class files inside the checked build directory.
foreach ($folder in @($classes,$dex,$generated)) {
    $resolved = [System.IO.Path]::GetFullPath($folder)
    if (-not $resolved.StartsWith($BuildDir + [System.IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Unexpected build path.' }
    Get-ChildItem -LiteralPath $resolved -Recurse -File | Remove-Item
}
$resources = Join-Path $BuildDir 'resources.zip'
$unsigned = Join-Path $BuildDir 'unsigned.apk'
$aligned = Join-Path $BuildDir 'aligned.apk'
Run-Tool $aapt2 @('compile','--dir',(Join-Path $android 'res'),'-o',$resources)
Run-Tool $aapt2 @('link','-I',$AndroidJar,'--manifest',(Join-Path $android 'AndroidManifest.xml'),'--java',$generated,'-o',$unsigned,$resources)
$javaSources = @(Get-ChildItem -LiteralPath (Join-Path $android 'src'),$generated -Recurse -Filter '*.java' | ForEach-Object FullName)
Run-Tool (Join-Path $JdkDir 'bin\javac.exe') (@('-J-Dfile.encoding=UTF-8','-encoding','UTF-8','-source','8','-target','8','-bootclasspath',((Join-Path $BuildToolsDir 'core-lambda-stubs.jar')+';'+$AndroidJar),'-d',$classes)+$javaSources)
$classFiles = @(Get-ChildItem -LiteralPath $classes -Recurse -Filter '*.class' | ForEach-Object FullName)
Run-Tool $java (@('-cp',(Join-Path $BuildToolsDir 'lib\d8.jar'),'com.android.tools.r8.D8','--min-api','26','--lib',$AndroidJar,'--output',$dex)+$classFiles)
Run-Tool (Join-Path $JdkDir 'bin\jar.exe') @('uf',$unsigned,'-C',$dex,'classes.dex')
Run-Tool (Join-Path $BuildToolsDir 'zipalign.exe') @('-p','-f','4',$unsigned,$aligned)
$key = Join-Path $BuildDir 'development.p12'
if (-not (Test-Path -LiteralPath $key)) {
    Run-Tool (Join-Path $JdkDir 'bin\keytool.exe') @('-genkeypair','-keystore',$key,'-storetype','PKCS12','-storepass','palmpower-local-dev','-keypass','palmpower-local-dev','-alias','palmpower','-keyalg','RSA','-keysize','2048','-validity','36500','-dname','CN=PalmPower Local Development','-noprompt')
}
$apk = Join-Path $release 'Android\GuanYuan.apk'
Run-Tool $java @('-jar',(Join-Path $BuildToolsDir 'lib\apksigner.jar'),'sign','--ks',$key,'--ks-pass','pass:palmpower-local-dev','--ks-key-alias','palmpower','--out',$apk,$aligned)
Run-Tool $java @('-jar',(Join-Path $BuildToolsDir 'lib\apksigner.jar'),'verify','--verbose',$apk)
# Validate the label actually packaged into the APK, decoding tool output explicitly as UTF-8.
# This catches source encoding corruption that a successful compile/signature check cannot find.
$labelCheck = New-Object System.Diagnostics.Process
$labelCheck.StartInfo.FileName = $aapt2
$labelCheck.StartInfo.Arguments = 'dump badging "'+$apk+'"'
$labelCheck.StartInfo.UseShellExecute = $false
$labelCheck.StartInfo.CreateNoWindow = $true
$labelCheck.StartInfo.RedirectStandardOutput = $true
$labelCheck.StartInfo.StandardOutputEncoding = [System.Text.UTF8Encoding]::new($false)
[void]$labelCheck.Start()
$badging = $labelCheck.StandardOutput.ReadToEnd()
$labelCheck.WaitForExit()
if ($labelCheck.ExitCode -ne 0 -or -not $badging.Contains("application-label:'"+$expectedName+"'")) { throw 'Packaged APK label is incorrect.' }
$labelCheck.Dispose()
Write-Output 'Verified packaged app label: U+5173 U+6E90'
Write-Output ('Built ' + $apk)
