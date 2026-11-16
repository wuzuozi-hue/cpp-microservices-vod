param(
    [string]$StateDir = (Join-Path $env:LOCALAPPDATA 'VodPlatformX11'),
    [int]$DisplayNumber = 0
)
$ErrorActionPreference = 'Stop'
if ($DisplayNumber -lt 0 -or $DisplayNumber -gt 999) { throw 'Invalid display number.' }
$projectDir = Split-Path -Parent $PSScriptRoot
$vcDir = Join-Path (Split-Path -Parent $projectDir) 'vcxsrv-user'
$server = Join-Path $vcDir 'vcxsrv.exe'
if (!(Test-Path -LiteralPath $server)) { throw 'VcXsrv user installation was not found.' }
if ($StateDir -match '[^\x20-\x7E]') { throw 'X11 state directory must use an ASCII-only path.' }
New-Item -ItemType Directory -Force -Path $StateDir | Out-Null
$xauth = Join-Path $StateDir 'xauth.exe'
if (!(Test-Path -LiteralPath $xauth)) { Copy-Item -LiteralPath (Join-Path $vcDir 'xauth.exe') -Destination $xauth }
Get-ChildItem -LiteralPath $vcDir -Filter '*.dll' | ForEach-Object {
    $destination = Join-Path $StateDir $_.Name
    if (!(Test-Path -LiteralPath $destination)) { Copy-Item -LiteralPath $_.FullName -Destination $destination }
}
function Test-DisplayAvailable([int]$number) {
    $probe = [Net.Sockets.TcpClient]::new()
    try { $probe.Connect('127.0.0.1', 6000 + $number); return $false }
    catch [Net.Sockets.SocketException] { return $true }
    finally { $probe.Dispose() }
}
if ($DisplayNumber -eq 0) {
    $DisplayNumber = @(41..80 | Where-Object { Test-DisplayAvailable $_ } | Select-Object -First 1)[0]
    if (!$DisplayNumber) { throw 'No free display port was found. Close unused client windows and try again.' }
} elseif (!(Test-DisplayAvailable $DisplayNumber)) {
    throw 'Display port is already in use; choose another display number.'
}
$auth = Join-Path $StateDir ("display-{0}.Xauthority" -f $DisplayNumber)
$display = "127.0.0.1:{0}.0" -f $DisplayNumber
$bytes = New-Object byte[] 16
$random = [Security.Cryptography.RandomNumberGenerator]::Create()
try { $random.GetBytes($bytes) } finally { $random.Dispose() }
$cookie = [BitConverter]::ToString($bytes).Replace('-', '').ToLowerInvariant()
& $xauth -f $auth remove $display 2>$null
& $xauth -f $auth add $display 'MIT-MAGIC-COOKIE-1' $cookie 2>$null
if ($LASTEXITCODE -ne 0) { throw 'Could not configure X11 authentication.' }
$serverProcess = $null
try {
    $serverProcess = Start-Process -FilePath $server -ArgumentList @(":$DisplayNumber", '-multiwindow', '-clipboard', '-listen', 'tcp', '-auth', ('"' + $auth + '"')) -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds 2
    if ($serverProcess.HasExited) { throw 'X11 server failed to start.' }
    $env:DISPLAY = $display
    $env:XAUTHORITY = $auth
    $xauthOption = 'XAuthLocation=' + $xauth.Replace('\', '/')
    Write-Host '请输入 dev-docker 的 SSH 密码；关闭播放器后本窗口会退出。'
    & ssh -X -tt -o $xauthOption -o StrictHostKeyChecking=yes dev-docker 'cd /home/dev/workspace/vod-platform && ./build/client/vod_player'
    if ($LASTEXITCODE -ne 0) { throw "SSH or client exited with status $LASTEXITCODE." }
}
finally {
    if ($serverProcess -and !$serverProcess.HasExited) { Stop-Process -Id $serverProcess.Id }
}
