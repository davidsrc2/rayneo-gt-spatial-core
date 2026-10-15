# Nimbo para Windows: plan de trabajo

Última actualización: 2026-10-14 (tarde). Objetivo: la misma experiencia que en macOS (monitores virtuales fijos en el espacio, panel compacto, conectar y olvidar) en Windows 11 x64.

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
| W3 | Implementada, **falta probarla**. Dos drivers, el que responda: *Virtual Display Driver* (VDD: MIT, firmado, se controla con `SETDISPLAYCOUNT`) o *virtual-display-rs* (MIT, el que ya tiene el PC de prueba: pipe `\\.\pipe\virtualdisplaydriver`, JSON + EOT, resolución exacta, respeta los monitores del usuario con ids < 41000). Monitores en cuadrícula, superficie de las gafas apartada, escala por monitor, cursor que sigue la mirada, ventanas traídas al panel 1, restauración al salir y `--restore` tras un cuelgue |
| W4 | **Hecha, falta probarla**: `apps/NimboApp` (WinUI 3, .NET 8, sin empaquetar). Bandeja + panel (luz de estado, interruptor, tarjetas de disposición, recentrar, deslizadores, perfiles, auto-inicio al conectar), ajustes (avanzado, editor de disposición, inicio con Windows, diagnóstico), asistente de primer uso (driver VDD, calibración guiada, tamaño del texto), avisos al conectar y desconectar, Ctrl+Alt+1/2/3, `settings.json` común con macOS. CI la publica como artefacto `nimbo-app-win-x64` |
| W1 | **Hecha**: `core/presets`, `core/panel_dimmer`, `core/frame_skip`, `core/settings` (los ajustes de la app de macOS, mismas claves, en JSON) con tests. `nimbo.dll` con C ABI (`rayneo/platform/nimbo_win.h`): arrancar y parar en su propio hilo, ajustes en vivo, estado, log, ajustes ↔ opciones, gafas y driver presentes, restaurar pantallas, calibración guiada. `nimbo_win.exe` es un CLI sobre esa API; `nimbo_api_test` la prueba en CI. Pendiente: que la app de macOS lea y escriba `settings.json` en vez de UserDefaults |

## Siguiente sesión (punto de continuación)

Estado al cerrar el 2026-10-14: W0, W1, W2, W3 y W4 en `main`, todo compila en CI (5 entornos, sin warnings) con 3 tests en Windows (`core_tests`, `windows_render_test` en WARP, `nimbo_api_test`). En el PC de prueba solo se ha visto funcionar W0 (IMU) y W2 en ventana (`--windowed --sim`).

Hechos del PC de prueba (Windows 11 Enterprise de Logista, Intel Iris Xe, monitores HP E24m G4 + HP E24 G5, gafas en `\\.\DISPLAY3`):
- Ya tiene **virtual-display-rs 0.4.0** (adaptador «Virtual Display», proveedor «Cherry», `ROOT\DISPLAY\0000`) con su pipe activo. VDD **no** está instalado; winget confunde ambos por el nombre.
- winget falla en consolas de administrador (fuente sin inicializar / alias sin registrar): usar consola normal o instalación manual.
- El build local está en `build-win` (`cmake --build build-win --config Release`).

Abierto en macOS tras las pruebas del usuario con las gafas (2026-10-15, MacBook Pro 14" M4 con notch):
- **Hecho y validado por el usuario:** firma local estable (los permisos ya no se pierden), panel de la barra de menús visible bajo el notch.
- **Hecho, sin validar:** atenuado desactivado por defecto (parecía un recorte junto al borde de las gafas), cursor que sigue a la mirada más conservador (0,8 s mirando otro panel; nunca tras un clic, una tecla o un movimiento del ratón), panel sin activar la app (sospecha: Nimbo activo + cursor físico oculto = cursor fuera de los paneles hasta un clic derecho).
- **Cursor que se pierde al hacer clic en «el panel de arriba»** (¿panel de Nimbo o barra de menús de su app?): con un clic derecho vuelve. Diagnóstico nuevo en `~/Library/Application Support/Nimbo/nimbo.log`: cada salto del cursor con su motivo y los cambios de app en primer plano. Pedir ese log.
- **«Pinned mode» de las gafas: el monitor izquierdo entero, el derecho cortado.** Aclarado: es el modo del firmware de las RayNeo, que fija la imagen por su cuenta y se suma a Nimbo. Con Nimbo las gafas van en modo **normal** (imagen pegada a la cabeza): 2D si «Imagen en 3D» está desactivado (por defecto), 3D lado a lado si está activado. Pendiente: avisarlo en el asistente de la app (macOS y Windows).
- **Vibración al subir y bajar la cabeza entre monitores; el usuario quiere cero vibración.** Probable causa: estaba usando el modo pinned de las gafas (dos compensaciones con sensores distintos). Primero repetir con las gafas en modo normal; si sigue, El log trae ahora `frame interval media±desv max` por segundo: si la desviación sube al mover la cabeza, es el ritmo de presentación (CAMetalDisplayLink 60–120 Hz, saltos de fotograma); si es estable, es la pose (seguimiento del `PoseStabilizer`, predicción). No ajustar a ciegas: medir primero con el log.

Hecho después (2026-10-15, validado por el usuario salvo lo marcado): modo pinned de las gafas soportado («Gafas en modo pinned»: Nimbo no mueve la imagen y encaja los monitores dentro de la que fijan las gafas; el usuario lo prefiere), seguimiento elegible (solo horizontal / solo vertical / ambos), atenuado desactivado por defecto. Sin validar: «Distancia» acerca y aleja de verdad (el arco se escala con la distancia; en pinned solo puede encoger), ⌘⌥C / Ctrl+Alt+C en pinned recentra las gafas con el comando `0x08` valor `01` (descubierto con el usuario: `0x08` = modo de imagen, 0 follow / 1 pinned / 2 steady; ver docs/PROTOCOL.md) y al empezar con «Gafas en modo pinned» Nimbo pone las gafas en pinned él mismo, atajo ⌘⌥N / Ctrl+Alt+N para abrir el panel. Todo igual en macOS y Windows.

Después: colocación a mano de los monitores (editor de arrastrar con imán y tamaño por monitor, `panels` en settings.json) y brillo de las gafas desde Nimbo (`0x09`, 1–8, panel y ⌘⌥[ ] / Ctrl+Alt+[ ]). Decisión del usuario: el modo de las gafas solo se cambia con su botón; la app no lo toca ni cambia su render por él (así distancia y tamaño funcionan en pinned). Queda un interruptor «Mis gafas están en modo pinned» que solo hace que ⌘⌥C / Ctrl+Alt+C recentre también las gafas (`0x08` valor `01`). El cursor físico solo se oculta si las gafas están en espejo (oculto, desaparecía de los paneles con Nimbo activo, p. ej. en «Más opciones»).

Después (2026-10-15): ventanas flotantes (una ventana como panel propio, por defecto encima de los monitores; macOS con ScreenCaptureKit, probado en modo ventana con un vídeo de YouTube encima de dos monitores virtuales; Windows con Windows.Graphics.Capture, solo compilado), perfiles por app (la app delante aplica la distancia / curvatura / tamaño de su perfil, temporal) y ⌘⌥J / Ctrl+Alt+J para llevar el cursor al monitor que miras. En la app de Mac: secciones «Ventanas flotantes» y «Perfiles por app» en Más opciones; en Windows, en Ajustes.

Después: modo enfoque (⌘⌥O / Ctrl+Alt+O: solo el panel que miras) y modo cine (⌘⌥P / Ctrl+Alt+P: ese panel centrado y 1,6 veces más grande); unas gafas transparentes no pueden oscurecer el mundo real, así que «cine» oculta el resto. Ventanas flotantes recordadas por app y título (se buscan de nuevo al empezar) y ⌘⌥J / Ctrl+Alt+J mirando una trae la ventana real al frente con el cursor. Safari deja de pintar el vídeo si su ventana no está visible en ninguna pantalla: dejarla en un monitor virtual.

Después: **pinned que sigue a la cabeza** (re-anclar la imagen al girar y pararse) se probó con las gafas y se **retiró**: re-anclaba a cada momento y movía las pantallas. Vuelve el encaje: todo (monitores y ventanas flotantes) dentro de la imagen fija de las gafas, sin nada que se mueva solo; la distancia y el tamaño solo pueden alejar, nunca agrandar más allá de la imagen (eso era lo que cortaba los bordes). La mirada en pinned usa los dos ejes, así que una ventana puesta arriba se elige mirando hacia arriba; la pose de Nimbo se pone a cero junto con el anclaje de las gafas (solo con ⌘⌥C / Ctrl+Alt+C: Nimbo nunca mueve el anclaje por su cuenta, ni al arrancar) para que mirada e imagen coincidan. **Cursor hasta las ventanas flotantes (solo macOS)**: cada ventana flotante va a un monitor virtual propio del tamaño de la ventana, colocado en la disposición de macOS donde flota (encima de los monitores → se llega subiendo el ratón), y se captura ese monitor (con cursor). Necesita Accesibilidad para mover la ventana; sin ella, captura de la ventana como antes. Windows: pendiente (hace falta un monitor extra del driver por ventana).

Pendiente, en orden:
0. **Validar con las gafas** el encaje en pinned (ventana arriba visible mirando hacia arriba, nada cortado a 1,9 m), la mirada vertical y el cursor en ventanas flotantes en macOS. Después, el cursor en ventanas flotantes en Windows.
1. **Probar W3 con virtual-display-rs** en el PC: `build-win\Release\nimbo_win.exe --windowed --sim --displays 2`. Riesgos sin verificar: el nombre que Windows da a sus monitores (se esperan «VirtuDisplay+»; si no, se detectan como pantallas nuevas), que acepte el JSON de `Notify` de la 0.4.0, que Windows admita las gafas tocando solo por una esquina (si no, van debajo del último panel) y el cambio de escala por monitor (llamada no documentada).
2. **Probar W2/W3 con las gafas** (`--displays 0` y `--displays 2`): `target err` cerca de 0 a pantalla completa, panel fijo al girar, texto nítido.
3. **Probar la app**: artefacto `nimbo-app-win-x64` del último build de `main` (o `dotnet publish`, ver W4). Asistente, calibración guiada, bandeja, Ctrl+Alt+1/2/3.
4. **W5 · Distribución** (preguntar al usuario antes de empezar): instalador con el driver VDD, firma Authenticode (requiere comprar certificado), actualizaciones (Velopack), instalador en CI.
5. Que la app de macOS use `settings.json` (`core/settings`) en vez de UserDefaults.

Forma de trabajar acordada: hasta la beta, commit y push directos a `main` (ver README, «Flujo de trabajo»); presentar cada paso siguiente y preguntar antes de empezarlo; respuestas en español.

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

#### Probar la app (4A)
Descargar el artefacto `nimbo-app-win-x64` del último build de `main` en GitHub Actions, descomprimir y abrir `Nimbo.exe` (lleva su `nimbo.dll` y el Windows App SDK dentro). Para compilarla en el PC hace falta el SDK de .NET 8 o superior:
```powershell
cmake --build build-win --config Release
dotnet publish apps\NimboApp\NimboApp.csproj -c Release -o build-win\NimboApp
build-win\NimboApp\Nimbo.exe
```
Aparece un icono en la bandeja (junto al reloj): clic para el panel, clic derecho para el menú. Sin gafas: definir `NIMBO_SIM=1` antes de abrirla (ventana + cabeza simulada). Registro en `%LOCALAPPDATA%\Nimbo\nimbo.log`.

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
