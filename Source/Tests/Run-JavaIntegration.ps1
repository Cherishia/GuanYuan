param([Parameter(Mandatory=$true)][string]$JdkDir,[Parameter(Mandatory=$true)][string]$Exe,[string]$BuildDir=(Join-Path $env:TEMP 'PalmPower-JavaTests'))
$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
$BuildDir=(Resolve-Path -LiteralPath $BuildDir).Path
$classes=Join-Path $BuildDir 'classes'
New-Item -ItemType Directory -Path $classes -Force | Out-Null
$protocol=Join-Path (Split-Path $PSScriptRoot -Parent) 'Android\src\local\palmpower\remote\Protocol.java'
& (Join-Path $JdkDir 'bin\javac.exe') '-J-Dfile.encoding=UTF-8' -encoding UTF-8 -d $classes $protocol (Join-Path (Split-Path $protocol -Parent) 'NtfyTransport.java') (Join-Path (Split-Path $protocol -Parent) 'RemoteClock.java') (Join-Path $PSScriptRoot 'ProtocolIntegration.java')
if($LASTEXITCODE -ne 0){throw 'Java test compilation failed'}
$fixture=Join-Path $BuildDir ([guid]::NewGuid().ToString('N')+'.json')
$process=Start-Process -FilePath (Resolve-Path -LiteralPath $Exe).Path -ArgumentList @('--integration-server',('"'+$fixture+'"')) -WindowStyle Hidden -PassThru
try {
    $until=[DateTime]::UtcNow.AddSeconds(10)
    while(-not(Test-Path -LiteralPath $fixture)){if([DateTime]::UtcNow -gt $until){throw 'Mock server startup timed out'};Start-Sleep -Milliseconds 100}
    & (Join-Path $JdkDir 'bin\java.exe') '-Dfile.encoding=UTF-8' -cp $classes ProtocolIntegration $fixture
    if($LASTEXITCODE -ne 0){throw 'Java / native Windows integration failed'}
} finally {
    New-Item -ItemType File -Path ($fixture+'.stop') -Force | Out-Null
    $process.WaitForExit(5000) | Out-Null
}
