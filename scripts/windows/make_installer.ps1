# Builds Nimbo-Setup-<version>.exe (Inno Setup 6) from a published app:
#   pwsh scripts/windows/make_installer.ps1 -AppDir build-win\NimboApp [-OutDir build-win\installer]
# Downloads the Virtual Display Driver (pinned release, checked by SHA-256) and the Visual C++
# runtime, then compiles installer/windows/Nimbo.iss. Installs Inno Setup with Chocolatey when
# ISCC.exe is not found.
param(
    [Parameter(Mandatory = $true)][string]$AppDir,
    [string]$OutDir = "build-win\installer",
    [string]$Version = ""
)
$ErrorActionPreference = "Stop"
$Root = Resolve-Path "$PSScriptRoot\..\.."
$AppDir = Resolve-Path $AppDir
foreach ($f in "Nimbo.exe", "nimbo_server.dll", "nimbo_win.exe") {
    if (-not (Test-Path (Join-Path $AppDir $f))) { throw "$f missing in $AppDir (publish the app first)" }
}
if (-not $Version) {
    $Version = ([xml](Get-Content "$Root\apps\NimboApp\NimboApp.csproj")).Project.PropertyGroup.Version | Where-Object { $_ } | Select-Object -First 1
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = Resolve-Path $OutDir
$Deps = Join-Path $OutDir "deps"
New-Item -ItemType Directory -Force -Path $Deps | Out-Null

# Virtual Display Driver 25.7.23 (MIT, VirtualDrivers/Virtual-Display-Driver), driver files only.
$VddZip = Join-Path $Deps "vdd.zip"
$VddUrl = "https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/download/25.7.23/VirtualDisplayDriver-x86.Driver.Only.zip"
$VddSha = "e24210692b442b39af763536330ce78b423f19342b7a7792c26de3944e418b3a"
if (-not (Test-Path $VddZip) -or (Get-FileHash $VddZip -Algorithm SHA256).Hash -ne $VddSha) {
    Invoke-WebRequest $VddUrl -OutFile $VddZip
}
if ((Get-FileHash $VddZip -Algorithm SHA256).Hash -ne $VddSha) { throw "Virtual Display Driver download does not match its SHA-256" }
$Driver = Join-Path $Deps "driver"
Remove-Item -Recurse -Force $Driver -ErrorAction SilentlyContinue
Expand-Archive $VddZip -DestinationPath (Join-Path $Deps "vdd") -Force
New-Item -ItemType Directory -Force -Path $Driver | Out-Null
Get-ChildItem (Join-Path $Deps "vdd") -Recurse -File | Where-Object { $_.Name -match '^(MttVDD\.(inf|dll|cat)|mttvdd\.cat|vdd_settings\.xml)$' } |
    Copy-Item -Destination $Driver
if (-not (Test-Path (Join-Path $Driver "MttVDD.inf"))) { throw "MttVDD.inf not found in the driver package" }

# Parsec VDD 0.45 (Parsec's virtual display driver, what Nimbo uses first): its own installer,
# pinned by SHA-256 and signed by Parsec. Redistributing it needs Parsec's permission.
$Parsec = Join-Path $Deps "parsec-vdd-0.45.0.0.exe"
$ParsecSha = "E23332448FDAF5AA017CB308DB5EF6855FAC526A7DED05D80C039404126D5362"
if (-not (Test-Path $Parsec) -or (Get-FileHash $Parsec -Algorithm SHA256).Hash -ne $ParsecSha) {
    Invoke-WebRequest "https://builds.parsec.app/vdd/parsec-vdd-0.45.0.0.exe" -OutFile $Parsec
}
if ((Get-FileHash $Parsec -Algorithm SHA256).Hash -ne $ParsecSha) { throw "Parsec VDD download does not match its SHA-256" }
if ((Get-AuthenticodeSignature $Parsec).Status -ne "Valid") { throw "Parsec VDD installer signature is not valid" }

# Visual C++ 2015-2022 runtime (x64): nimbo_server.dll needs it. Microsoft-signed.
$Redist = Join-Path $Deps "vc_redist.x64.exe"
if (-not (Test-Path $Redist)) { Invoke-WebRequest "https://aka.ms/vs/17/release/vc_redist.x64.exe" -OutFile $Redist }
$sig = Get-AuthenticodeSignature $Redist
if ($sig.Status -ne "Valid" -or $sig.SignerCertificate.Subject -notmatch "Microsoft") { throw "vc_redist.x64.exe is not signed by Microsoft" }

$Iscc = (Get-Command iscc.exe -ErrorAction SilentlyContinue).Source
if (-not $Iscc) { $Iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" }
if (-not (Test-Path $Iscc)) {
    choco install innosetup -y --no-progress | Out-Null
    $Iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
}
& $Iscc /Q "/DAppDir=$AppDir" "/DDriverDir=$Driver" "/DRedist=$Redist" "/DParsecVdd=$Parsec" "/DVersion=$Version" "/DOutputDir=$OutDir" `
    "$Root\installer\windows\Nimbo.iss"
if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)" }
Write-Host "Installer: $OutDir\Nimbo-Setup-$Version.exe"
