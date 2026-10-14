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
| W0 | Preparada: hilo del IMU con MMCSS + temporizador de 1 ms en Windows; `scripts/windows/w0_check.ps1` comprueba el PC, compila y mide la IMU. **Falta ejecutarlo en el PC con las gafas** |
| W1 | Hecha en parte: `core/presets`, `core/panel_dimmer`, `core/frame_skip` con tests; macOS los usa; plantillas y perfiles expuestos en `rayneo_layouts.h` (C ABI). Pendiente: modelo de opciones en JSON común y ampliar el C ABI para la app de Windows |

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
1. `nimbo_win.exe` en CLI con `--windowed --sim`: D3D11, shaders HLSL portados de `Shaders.metal`, mipmaps (`GenerateMips`), vistas sRGB, half-SBS.
2. Captura DXGI Desktop Duplication de una pantalla existente y copia a textura propia.
3. Ventana sin bordes a pantalla completa en la salida de las gafas, flip model a 120 Hz, excluida de la captura.
4. Predicción hasta el scanout con `IDXGISwapChain::GetFrameStatistics`; log de latencia como en macOS.
5. Cursor: Desktop Duplication entrega el puntero aparte, así que hay que componerlo en el render.

**Hecho cuando:** con las gafas, una pantalla fija en el espacio a 120 Hz, texto nítido y latencia medida.

### W3 · Monitores virtuales (L) — el mayor riesgo
1. Evaluar un driver IddCx de código abierto (p. ej. *Virtual Display Driver*, licencia MIT) frente a escribir uno propio con el ejemplo IddCx de Microsoft.
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
