# Nimbo para Windows: plan de trabajo

Última actualización: 2026-10-14. Objetivo: la misma experiencia que en macOS (monitores virtuales fijos en el espacio, panel compacto, conectar y olvidar) en Windows 11 x64.

## Punto de partida

| Pieza | Estado en Windows |
|---|---|
| `core/` (protocolo GT, Madgwick, bias, estabilizador, recentrado suave, matemáticas de render) | Portable. Compila y pasa los tests en CI (MSVC) |
| `hal/` (hidapi + hilo lector + ring SPSC) | Compila en CI. **Sin probar con las gafas** |
| `platform/rayneo_engine.h` (C ABI) | Portable. Es la frontera para la app de Windows |
| `apps/imu_probe` | Compila en Windows: primera prueba con hardware |
| Captura, render, monitores virtuales, cursor, atajos, UI (`apps/nimbo`, `src/platform/macos`) | Solo macOS: hay que reescribirlo |

Equivalencias:

| macOS | Windows |
|---|---|
| ScreenCaptureKit | **DXGI Desktop Duplication** (latencia mínima, por salida) o Windows.Graphics.Capture |
| Metal + `CAMetalDisplayLink` | **Direct3D 11**, swap chain flip model con *waitable object* a 120 Hz |
| `CGVirtualDisplay` (API privada) | **Driver de pantalla indirecta (IddCx)**: no hay API pública sin driver |
| `sharingType = .none` (ocultar el render de la captura) | `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` |
| Modo nativo, espejo/extender, restaurar | `QueryDisplayConfig` / `SetDisplayConfig` / `ChangeDisplaySettingsEx` |
| Atajos Carbon (⌘⌥) | `RegisterHotKey` (Ctrl+Alt+C, Ctrl+Alt+1/2/3…) |
| Barra de menús + popover | Icono de bandeja + ventana flotante con Mica |
| Accesibilidad (mover ventanas) | `SetWindowPos` (sin permiso especial) |
| Centro de notificaciones | Toasts de Windows (requiere AppUserModelID) |
| DMG + notarización | Instalador MSIX o Inno Setup + firma Authenticode |

## Arquitectura propuesta

```
NimboWin (C#, .NET 8, WinUI 3)        bandeja, panel, asistente, ajustes, toasts
        │ P/Invoke (C ABI)
nimbo_win.dll (C++17)                  captura DXGI, render D3D11, pantallas, cursor, atajos
        │
rayneo_engine (core + hal, ya existe)  IMU, fusión, estabilizador, recentrado
        +
Driver IddCx (monitores virtuales)     instalado aparte, con firma
```

- **UI en C# + WinUI 3:** encaja con tu experiencia en .NET y da el aspecto de cristal (Mica) del panel de macOS. Alternativa más conservadora: WPF.
- **Núcleo nativo en C++:** captura y render necesitan latencia mínima y comparten los tipos de `core/render_math.h`.
- **Lógica compartida al core:** antes de duplicarla en C#, mover a C++ lo que hoy vive en Swift/Obj-C++ y no depende del SO: plantillas de disposición, mirada → panel, atenuado con fundido, decisión de saltar fotogramas, modelo de opciones (JSON común).

## Estado

| Fase | Estado |
|---|---|
| W0 | **Hecha** (2026-10-14, Win 11 Enterprise, Intel Iris Xe): gafas como pantalla DP, HID 3941:af50, build MSVC 2022, IMU a 457.19 Hz, missed 0, irregular 0, ACK sí; pose en vivo sigue yaw/pitch/roll |
| W2 | Probada en ventana (`--windowed --sim`) tras corregir el bloqueo entre captura y render. **Falta el log con las gafas**: `nimbo_win.exe` (captura DXGI con cursor, render D3D11 portado de Metal, swap chain flip a 120 Hz excluida de la captura, predicción hasta el scanout). CI compila sin warnings y renderiza un fotograma en WARP (`windows_render_test`) |
| W3 | Implementada sobre *Virtual Display Driver* (MIT, IddCx, firmado), **falta probarla**: monitores virtuales en cuadrícula, superficie de las gafas apartada, escala por monitor, cursor que sigue la mirada, ventanas traídas al panel 1, restauración al salir y `--restore` tras un cuelgue |
| W1 | **Hecha**: `core/presets`, `core/panel_dimmer`, `core/frame_skip`, `core/settings` (los ajustes de la app de macOS, mismas claves, en JSON) con tests. `nimbo.dll` con C ABI (`rayneo/platform/nimbo_win.h`): arrancar y parar en su propio hilo, ajustes en vivo, estado, log, ajustes ↔ opciones, gafas y driver presentes, restaurar pantallas, calibración guiada. `nimbo_win.exe` es un CLI sobre esa API; `nimbo_api_test` la prueba en CI. Pendiente: que la app de macOS lea y escriba `settings.json` en vez de UserDefaults |

## Fases

Tamaños: S = días, M = 1–2 semanas, L = 3+ semanas.

### W0 · Preparación y prueba de hardware (S) — empezar ya

#### Guía paso a paso (en el PC con Windows)

**1. Instalar herramientas** (una vez, PowerShell):
```powershell
winget install Git.Git Kitware.CMake
winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```
Cierra y abre PowerShell después, para que encuentre `git` y `cmake`. Si ya tienes Visual Studio 2022, basta con que tenga «Desarrollo para el escritorio con C++».

**2. Bajar el código:**
```powershell
git clone https://github.com/davidsrc2/rayneo-gt-spatial-core
cd rayneo-gt-spatial-core
```
Si ya estaba clonado: `git checkout main; git pull`.

**3. Conectar las gafas** a un USB-C que saque vídeo (suele llevar el icono DP o el del rayo). Deben verse como una pantalla más en Configuración > Pantalla. Déjalas **quietas sobre la mesa**.

**4. Ejecutar la comprobación** (desde la carpeta del repo):
```powershell
powershell -ExecutionPolicy Bypass -File scripts\windows\w0_check.ps1
```
Tarda unos minutos la primera vez (compila). Todo queda en `w0_report.txt`.

**5. Leer el resultado:**

| Línea | Bien | Si no |
|---|---|---|
| `pantalla de las gafas detectada` | OK en verde | Ese USB-C no saca vídeo: prueba otro puerto o cable. Sin esto no hay W2 |
| `dispositivo USB de las gafas presente` | OK en verde | Cable o puerto: el USB de datos no llega |
| `device-clock rate` | ~457 Hz | Anotar el valor |
| `missed` | 0 | Pérdidas: anotar; se mira la prioridad del hilo |
| `ack` | 1 o más | Las gafas no respondieron al comando de stream |

**6. Opcional:** `build-win\Release\imu_probe.exe` muestra la pose en vivo; mueve la cabeza con las gafas puestas y comprueba que yaw/pitch/roll siguen el movimiento (Ctrl+C para salir).

**7. Mandar `w0_report.txt`** (o pegar su contenido) para empezar la W2.

#### Qué valida

1. **Comprobar el PC:** USB-C con **DisplayPort Alt Mode** (las gafas son una pantalla DP; sin eso no hay imagen) y GPU con D3D11.1. Revisar el mini PC.
2. Compilar con Visual Studio 2022 / CMake: `cmake -S . -B build -G "Visual Studio 17 2022"`.
3. `imu_probe --list`, `--raw` y medir cadencia: confirmar **457 Hz**, sin pérdidas, ACK. Puntos a vigilar en Windows: prefijo de report ID 0 de hidapi en escritura, prioridad del hilo lector (MMCSS «Pro Audio»), `timeBeginPeriod(1)`.
4. `imu_probe --calibrate` con las gafas: la calibración `sensor_to_head` es la misma.

**Hecho cuando:** pose estable a 457 Hz en Windows, igual que en el Mac.

### W1 · Mover la lógica compartida al core (M)
1. Plantillas de disposición, opciones y su JSON a `core/` con tests.
2. Atenuado con fundido y «nada ha cambiado» (salto de fotogramas) a `core/`, con tests. El renderer Metal pasa a usarlos.
3. Ampliar el C ABI para lo que necesita la app de Windows (opciones, mirada, estado).

**Hecho cuando:** el Mac sigue igual y los tests nuevos pasan en los 3 SO.

### W2 · Render espacial con una pantalla real (L)
Equivale a la fase 3 de macOS (`--displays 0`).

#### Guía de prueba (en el PC con Windows)

**1. Actualizar y compilar** (desde la carpeta del repo):
```powershell
git pull
cmake --build build-win --config Release
```

**2. Sin las gafas puestas, en una ventana** (comprueba captura y render):
```powershell
build-win\Release\nimbo_win.exe --windowed --sim --sim-sweep 20
```
Debe abrirse una ventana con tu pantalla principal como un panel que se mueve a izquierda y derecha. El cursor debe verse dentro del panel. Ctrl+Alt+Q para salir.

**3. Con las gafas** (en modo 3D/SBS de las gafas, como en el Mac):
```powershell
build-win\Release\nimbo_win.exe
```
Deja las gafas quietas sobre la mesa ~6 s hasta que el log pase de `CALIB` a `TRACK`, póntelas y pulsa Ctrl+Alt+C para recentrar.

**4. Qué mirar:**

| Línea / prueba | Bien |
|---|---|
| `glasses mode:` | 1920x1080 @120 Hz (o el modo nativo que ofrezcan) |
| `render NNN fps` | ~120 moviendo la cabeza; con la cabeza quieta baja (`idle`): es normal |
| `sample->scanout` | Latencia desde la muestra de la IMU hasta el scanout; anotar media/máx |
| `target err` | Cerca de 0: la predicción apunta al vblank correcto. Si sale ~+8 ms, hay un fotograma de composición extra |
| Girar la cabeza | El panel se queda fijo. Si se mueve **con** la cabeza: Ctrl+Alt+-; si va **en contra**: Ctrl+Alt+= (se guarda) |
| Texto | Nítido, sin parpadeo al mover la cabeza un poco |

**5. Mandar el log** de la consola (unos 10 s en `TRACK`) y cómo se ve.

Opciones útiles: `--source N` elige qué pantalla mostrar (índice de la lista `displays:` del log), `--mono` si las gafas están en 2D, `--arc 40` panel más pequeño, `--60hz`, `--help`.

#### Tareas
1. `nimbo_win.exe` en CLI con `--windowed --sim`: D3D11, shaders HLSL portados de `Shaders.metal`, mipmaps (`GenerateMips`), vistas sRGB, half-SBS.
2. Captura DXGI Desktop Duplication de una pantalla existente y copia a textura propia.
3. Ventana sin bordes a pantalla completa en la salida de las gafas, flip model a 120 Hz, excluida de la captura.
4. Predicción hasta el scanout con `IDXGISwapChain::GetFrameStatistics`; log de latencia como en macOS.
5. Cursor: Desktop Duplication entrega el puntero aparte, así que hay que componerlo en el render.

**Hecho cuando:** con las gafas, una pantalla fija en el espacio a 120 Hz, texto nítido y latencia medida.

### W3 · Monitores virtuales (L) — el mayor riesgo

#### Decisión: Virtual Display Driver
[VirtualDrivers/Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver): IddCx, licencia MIT, **firmado** (SignPath), sin *test signing*. Se controla por su pipe `\\.\pipe\MTTVirtualDisplayPipe` (`SETDISPLAYCOUNT n` reescribe `C:\VirtualDisplayDriver\vdd_settings.xml` y recarga el adaptador). Sus monitores se llaman «VDD by MTT». El driver mantiene siempre al menos uno: Nimbo lo deja desconectado al salir. Las resoluciones posibles son las de `vdd_settings.xml` (por defecto 1920x1080, 2560x1440, 3840x2160).

El adaptador «Virtual Display» (driver 0.4.0.0) que ya tiene el PC de prueba no es este driver. Para ver qué es: `Get-PnpDevice -Class Display | Format-List FriendlyName,Manufacturer,InstanceId,Status`.

#### Guía de prueba
**1. Instalar el driver** (una vez, requiere administrador): descargar la última versión de *Releases* del repositorio e instalarla (VDD Control o el paquete *Driver Only* con su instrucción). En un PC de empresa, comprobar antes que la política de IT permite instalar drivers.

**2. Actualizar y compilar:**
```powershell
git pull
cmake --build build-win --config Release
```

**3. Sin gafas, en ventana:**
```powershell
build-win\Release\nimbo_win.exe --windowed --sim --displays 2
```
Aparecen dos monitores nuevos a la derecha de los tuyos (Configuración > Pantalla) y la ventana los muestra como dos paneles. Ctrl+Alt+flechas para mirar a cada uno. Al salir (Ctrl+Alt+Q) desaparecen y todo vuelve a su sitio.

**4. Con las gafas:**
```powershell
build-win\Release\nimbo_win.exe --displays 2
```

| Qué | Bien |
|---|---|
| `virtual monitors:` | Dos monitores con posición y tamaño, escala 150 % |
| Ventanas | Las que estaban en las gafas pasan al panel 1 |
| Mirar al otro panel | El cursor salta a él tras ~1,5 s sin mover el ratón |
| Salir | `displays: restored, virtual monitors off`; tus pantallas como estaban |

Variantes: `--layout "0,0;0,1"` (uno encima de otro), `--displays 3`, `--fit`, `--dim 0.6`, `--vd-size 2560x1440`, `--vd-scale 175`.

**Si algo se queda mal** (cuelgue, corte de luz): `build-win\Release\nimbo_win.exe --restore`. También se hace solo al arrancar la siguiente vez.

#### Tareas
1. Evaluar un driver IddCx de código abierto (p. ej. *Virtual Display Driver*, licencia MIT) frente a escribir uno propio con el ejemplo IddCx de Microsoft. Nota: el PC de prueba ya tiene un adaptador «Virtual Display» (driver 0.4.0.0); identificar cuál es antes de instalar otro.
2. Crear y quitar monitores desde `nimbo_win.dll` (control del driver), resolución HiDPI y disposición en fila o cuadrícula.
3. Multi-panel, mirada → cursor, traer ventanas al panel 1 (`SetWindowPos`), restaurar todo al salir o al desconectar.

**Hecho cuando:** 2–3 monitores virtuales con las gafas y vuelta limpia al estado original.

### W4 · App (M)
1. Bandeja + panel WinUI 3: estado, interruptor, tarjetas 1/2/3, recentrar, deslizadores, perfiles, auto-inicio.
2. Asistente: comprobación de driver, calibración guiada, escala.
3. Atajos Ctrl+Alt+C / Q / 1 / 2 / 3, toasts al conectar y desconectar, arranque con Windows.

### W5 · Distribución (M)
1. Instalador que incluye el driver (requiere admin). Firma: certificado Authenticode (p. ej. Azure Trusted Signing) para la app; el driver necesita **firma de atestación de Microsoft** vía Partner Center (cuenta de hardware y certificado EV).
2. Actualizaciones automáticas (Velopack para .NET).
3. CI: build de `nimbo_win` y de la app, artefacto del instalador.

## Riesgos

| Riesgo | Impacto | Mitigación |
|---|---|---|
| Firma del driver IddCx | Sin firma solo funciona con *test signing* | Empezar el trámite (cuenta + EV) en W0; mientras, desarrollo con test signing o un driver ya firmado |
| PC sin DP Alt Mode | Las gafas no se ven | Comprobarlo en W0 antes de nada |
| Portátiles con dos GPU | Captura en una GPU y render en otra: copias extra | Detectar el adaptador de cada salida; copia entre adaptadores solo si hace falta |
| Contenido protegido (DRM) | Paneles en negro con vídeo protegido | Documentarlo |
| hidapi en Windows con informes sin ID | Lecturas o ACK incorrectos | W0 lo valida con las gafas |

## Orden recomendado
W0 en paralelo con el inicio del trámite de firma del driver → W2 (lo más visible: una pantalla fija en las gafas) → W1 cuando W2 necesite la lógica compartida → W3 → W4 → W5.
