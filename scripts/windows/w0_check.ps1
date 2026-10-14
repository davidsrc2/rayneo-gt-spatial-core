# W0 on Windows: checks the PC, builds the core and measures the glasses' IMU.
# Run from the repo root in PowerShell:  powershell -ExecutionPolicy Bypass -File scripts\windows\w0_check.ps1
# Writes everything to w0_report.txt as well, to share the results.
param([int]$RawSeconds = 20)

$ErrorActionPreference = 'Continue'
$report = Join-Path (Get-Location) 'w0_report.txt'
Start-Transcript -Path $report -Force | Out-Null

function Section($t) { Write-Host "`n=== $t ===" -ForegroundColor Cyan }
function Ok($t)      { Write-Host "  OK   $t" -ForegroundColor Green }
function Warn($t)    { Write-Host "  FALTA $t" -ForegroundColor Yellow }

Section 'Sistema'
$os = Get-CimInstance Win32_OperatingSystem
Write-Host "  $($os.Caption) $($os.Version) $($env:PROCESSOR_ARCHITECTURE)"
Get-CimInstance Win32_VideoController | ForEach-Object { Write-Host "  GPU: $($_.Name) (driver $($_.DriverVersion))" }

Section 'Pantallas (las gafas se anuncian como SmartGlasses por DisplayPort)'
# VideoOutputTechnology: 10 = DisplayPort externo, 5 = HDMI, 0x80000000 = interna.
$conn = @{}
Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorConnectionParams -ErrorAction SilentlyContinue |
    ForEach-Object { $conn[$_.InstanceName] = $_.VideoOutputTechnology }
$glassesDisplay = $false
Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorID -ErrorAction SilentlyContinue | ForEach-Object {
    $name = -join ($_.UserFriendlyName | Where-Object { $_ -ne 0 } | ForEach-Object { [char]$_ })
    $tech = $conn[$_.InstanceName]
    Write-Host "  '$name' salida=$tech"
    if ($name -match 'SmartGlasses|RayNeo') { $glassesDisplay = $true }
}
if ($glassesDisplay) { Ok 'pantalla de las gafas detectada (el USB-C saca video)' }
else { Warn 'pantalla de las gafas: conecta las gafas a un USB-C con DisplayPort Alt Mode (icono DP o rayo)' }
Add-Type -AssemblyName System.Windows.Forms
[System.Windows.Forms.Screen]::AllScreens | ForEach-Object {
    Write-Host "  pantalla $($_.DeviceName) $($_.Bounds.Width)x$($_.Bounds.Height) principal=$($_.Primary)"
}

Section 'USB / HID de las gafas (VID 3941, PID AF50)'
$hid = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'VID_3941&PID_AF5' }
if ($hid) { $hid | ForEach-Object { Write-Host "  $($_.Class) $($_.FriendlyName) [$($_.Status)] $($_.InstanceId)" }; Ok 'dispositivo USB de las gafas presente' }
else { Warn 'no se ve el USB de las gafas' }

Section 'Herramientas'
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
if ($cmake) { Ok "cmake $((cmake --version)[0])" } else { Warn 'cmake (winget install Kitware.CMake)' }
if ($vs) { Ok "Visual Studio C++: $vs" } else { Warn 'Visual Studio 2022 o Build Tools con "Desarrollo para el escritorio con C++"' }
if (-not ($cmake -and $vs)) { Stop-Transcript | Out-Null; exit 1 }

Section 'Build (Release)'
cmake -S . -B build-win -G 'Visual Studio 17 2022' -A x64 | Select-Object -Last 2
cmake --build build-win --config Release --parallel | Select-Object -Last 3
ctest --test-dir build-win -C Release --output-on-failure | Select-Object -Last 2
$probe = 'build-win\Release\imu_probe.exe'
if (-not (Test-Path $probe)) { Warn "no se genero $probe"; Stop-Transcript | Out-Null; exit 1 }

Section 'IMU: dispositivos'
& $probe --list

Section "IMU: $RawSeconds s de stream (deja las gafas quietas sobre la mesa)"
# Esperado en el hardware de referencia: device-clock rate ~457 Hz, missed 0, ACK presente.
& $probe --raw $RawSeconds

Section 'Siguiente paso'
Write-Host '  Para la pose fusionada: build-win\Release\imu_probe.exe   (Ctrl+C para salir)'
Write-Host "  Informe guardado en $report"
Stop-Transcript | Out-Null
