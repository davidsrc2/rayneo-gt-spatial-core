# Nimbo · rayneo-gt-spatial-core

Motor 3DoF multiplataforma para las gafas RayNeo GT (USB HID, ~457 Hz). PoC para un gestor de ventanas espaciales en macOS (Apple Silicon); el núcleo es portable a Windows.

## Flujo de trabajo (hasta la beta)

Hasta tener una beta, todo se sube directamente a `main`: commit y push sin ramas ni PR, para poder probar siempre la última versión con un `git pull`. El CI corre en cada push a `main`; si falla, se arregla en el siguiente commit. Al publicar la beta se vuelve a ramas `feat/...` + PR con CI en verde.

## Permisos de macOS estables (una vez por Mac)

Con firma «ad hoc», cada compilación cambia la identidad de la app y macOS vuelve a pedir Grabación de pantalla y Accesibilidad (y los permisos viejos dejan la app a medias). Para evitarlo:
```
./scripts/make_signing_identity.sh   # certificado local "Nimbo Local Signing" en el llavero
cmake -S . -B build && cmake --build build
tccutil reset ScreenCapture io.github.davidsrc2.nimbo; tccutil reset Accessibility io.github.davidsrc2.nimbo
```
Después se conceden los permisos una sola vez y se mantienen al recompilar o reinstalar.

## Estado del proyecto

Última actualización: 2026-10-14. Hardware de referencia: RayNeo GT conectadas por USB-C a un MacBook Pro M4 (macOS 27.2). Cuando el hardware contradice la documentación, manda el hardware.

**Objetivo del producto:** usar las gafas como un escritorio de varios monitores virtuales fijos en el espacio. Miras a un monitor u otro girando la cabeza, el texto se lee cómodo y la imagen está tan quieta como la de un monitor normal.

| Fase | Contenido | Estado |
|---|---|---|
| 1 | HAL en macOS M4: build arm64 sin warnings, hidapi estático, `imu_probe --list/--raw`, `TickMonitor` | Fusionada (#1). Validada con las gafas |
| 2 | Escape y rollback del bias, calibración `sensor_to_head` (`imu_probe --calibrate`) | Fusionada (#2) |
| fixes | Calibración con las gafas puestas (#3), cabeceo con giro mezclado (#5), tracking con las gafas puestas desde el arranque (#6), deriva de yaw con las gafas puestas (#7) | Fusionadas. Calibración validada con las gafas (métricas 4,6° / 3,5° / 1,6°) |
| 3 | Servidor Metal: ScreenCaptureKit, render half-SBS/mono, predicción hasta el scanout | Rama `feat/phase3-macos-renderer` |
| 3b | Escritorio de monitores virtuales (`CGVirtualDisplay`), paneles planos, estabilizador con zona muerta, un solo cursor, cursor que sigue la mirada, barra de menús ◎ RayNeo | Rama `feat/phase3b-virtual-displays` (encima de la 3). En prueba con las gafas |
| 4 | CI en GitHub Actions: macOS arm64, Windows MSVC, Linux GCC/Clang + aislamiento de core/hal | Fusionada (#4). Verde en los 3 SO |
| 5 | **Interfaz gráfica** SwiftUI: disposición en cuadrícula (horizontal y vertical), plantillas, vista previa 3D, tamaño de texto / distancia / separación / forma, asistente (permisos, calibración, escala), Ajustes con Avanzado, menú ◎ | Rama `feat/phase5-gui` (encima de la 3b). PR abierta |

### Qué funciona hoy con las gafas (validado por el usuario)

- Calibración guiada, recentrado (`Cmd+Opt+C` y menú ◎ RayNeo), dos monitores virtuales visibles.
- Sin deriva apreciable tras los arreglos #6 y #7.
- Desconectar las gafas cierra el servidor y restaura las pantallas.
- Letra del Mac en 1280×720 HiDPI (ajuste del sistema).

### Pendiente de afinar con las gafas

- **Escala angular:** la imagen parece moverse más despacio que la cabeza, porque el FOV de render (24,5°) es una estimación. Se ajusta en vivo con `Cmd+Opt+-` / `Cmd+Opt+=` hasta que los monitores no se muevan al girar, y queda guardado.
- **Sensación del estabilizador:** al volver a quedarse fija, la imagen hace un gesto algo forzado. Zona muerta reducida a 1,0° y seguimiento más rápido; pendiente de validar.
- **Letra más grande en los monitores virtuales:** por defecto 1152×648 pt HiDPI; `--vd-size 1024x576` para más grande.

### Medidas sobre el hardware de referencia

- **Informe IMU:** 64 B sin report ID; el ACK `99 c8` llega en ~1 ms.
- **Frecuencia real del sensor:** **457,14 Hz** (deltas de tick: 22 siete de cada ocho, 21 la restante), no los 476 documentados.
- **Estabilidad del stream:** 0 pérdidas y 0 drops del ring. Bias en reposo ~0,4 °/s. El IMU va montado inclinado ~29° en la montura.
- **Pantalla `SmartGlasses`:** 1920×1080 a 60/120 Hz, en espejo con la pantalla interna; sin modo 3840×1080 (estéreo = half-SBS).
- **Latencia muestra IMU → scanout:** 26,4 ms a 120 Hz (suelo de WindowServer: edad de pose 1,4 ms + 3 refrescos), compensada con predicción solo al girar.

### Riesgos conocidos

- `CGVirtualDisplay` y el ocultado de cursor en segundo plano son APIs privadas: pueden romperse con una actualización de macOS y la app no pasaría la revisión de la App Store.
- Distorsión de lente desactivada (`k1 = k2 = 0`): no se conoce la óptica de las GT.
- Traer las ventanas abiertas a los monitores virtuales requiere el permiso de Accesibilidad.

## Usar la app (fase 5)

Instalar como cualquier app de Mac:

```sh
cd ~/rayneo-gt-spatial-core && git pull && scripts/make_dmg.sh   # -> dist/Nimbo-<versión>.dmg
open dist/Nimbo-*.dmg                                     # arrastrar «Nimbo» a Aplicaciones
```

Después se abre desde Launchpad / Spotlight («Nimbo»). Para desarrollo también vale `open build/Nimbo.app`.

- Sin argumentos se abre como **app de barra de menús**: clic en **◎** abre el panel compacto (cristal nativo, oscuro). Clic derecho: menú rápido.
  - Arriba, el estado tipo semáforo (verde «Sensores estabilizados», ámbar «Calibrando (deja las gafas en la mesa 5 s)», gris apagado) y el interruptor de encendido.
  - Tarjetas de un clic: **1 Pantalla Curva**, **Doble Monitor**, **Triple Ultrawide**. Con las gafas siguiendo la cabeza, la tarjeta activa ilumina el monitor que estás mirando. Atajos globales **⌘⌥1 / ⌘⌥2 / ⌘⌥3** sin abrir el panel (con un aviso nativo).
  - **Recentrar vista** (⌘⌥C; no ⌘⇧R porque es «recargar» en Safari y Chrome y un atajo global lo anularía). La imagen se desliza a su sitio en 300 ms en vez de saltar.
  - Deslizadores con anclajes: **Distancia** (Cerca / Normal / Lejos), **Curvatura** continua (Plana / Suave / Envolvente; `core::bent_screen_mesh`), **Tamaño**.
  - Perfiles **Modo Código** y **Modo Cine**, y **Auto-iniciar al conectar las RayNeo GT**.
  - «Más opciones…» abre la ventana completa: editor de cuadrícula (horizontal y vertical), tamaño de texto, Ajustes y Avanzado.
- Avisos en el centro de notificaciones al conectar y desconectar las gafas.
- Ajustes > Pantallas: **Atenuar los monitores que no miras** (activado por defecto; `--dim 0..1` en CLI).
- Al pulsar Empezar el panel sigue respondiendo mientras se preparan los monitores (2–3 s); se puede cancelar apagando el interruptor.
- Con flags sigue funcionando como CLI (`--help`; `--curvature 0..1`).
- Primera vez: asistente con permisos (Grabación de pantalla y Accesibilidad **para la app**, no para Terminal), calibración guiada (reutiliza tu `calibration.json` si ya existía) y ajuste de escala.
- Disposición: arrastra las tarjetas para poner monitores al lado o encima/debajo; `+`/`−`; plantillas. El monitor 1 es el principal de macOS (barra de menús y Dock).
- Distancia, separación, forma, FOV, IPD, 3D y lente se aplican en vivo; el número de monitores, la disposición, el tamaño del texto y la zona quieta relanzan el servidor.
- Menú ◎ en la barra superior: Recentrar, Empezar/Parar, Abrir disposición, Salir.
- Ajustes y calibración en `~/Library/Application Support/RayNeo/` y en las preferencias de la app.
- La firma es ad-hoc: tras recompilar, macOS puede volver a pedir los permisos.
- SwiftUI sin la macro `@State` (su plugin solo viene con Xcode; el build usa las Command Line Tools).

## Hoja de ruta macOS (recomendaciones)

**Imprescindible antes de publicar la v1.0**
1. Cerrar lo pendiente con las gafas: FOV definitivo como valor por defecto, monitores apilados, asistente; fusionar la PR #8.
2. Firma Developer ID + notarización (sin avisos de Gatekeeper).
3. Actualizaciones automáticas (Sparkle) para la distribución por DMG.
4. Abrir al iniciar sesión (`SMAppService`): junto con el auto-inicio, «conectar y olvidar» completo.
5. «Exportar diagnóstico»: zip con registros, frecuencia de la IMU, latencia, calibración y versión de macOS.
6. Grabar y reproducir sesiones de la IMU en los tests, para validar estabilizador y deriva sin hardware (acelera también Windows).

**Mejoran la experiencia**
7. Recordar dónde va cada app (editor en el monitor 1, navegador en el 2…).
8. Modo «seguir» para caminar o levantarse: los monitores acompañan despacio.
9. Capturar a pocos fps los monitores fuera de la vista (batería y temperatura).
10. Corrección de deriva opcional con el magnetómetro, con filtro de interferencias.
11. Recentrar con un gesto (doble asentimiento).
12. Calibración de lente con patrón de rejilla (k1/k2).

**Más adelante**
13. 3D real (SBS) en Modo Cine: hecho en macOS y Windows como «Cine en 3D con IA» (ver docs/WINDOWS_PLAN.md, siguiente 4).
14. Inglés y VoiceOver.
15. Atajos configurables.

Orden sugerido: 1 → 6 → 4 → 5; 2 y 3 con la cuenta de desarrollador.

## Icono y distribución (DMG)

- Icono propio (no el logo de RayNeo/TCL): tres pantallas en arco sobre unas gafas, paleta de la app. Generado por código: `swift scripts/make_icons.swift assets/icon && iconutil -c icns assets/icon/AppIcon.iconset -o assets/icon/AppIcon.icns`. Incluye el icono de barra de menús (*template*, se adapta a claro/oscuro).
- DMG para instalar arrastrando a Aplicaciones:
  ```sh
  scripts/make_dmg.sh                       # -> dist/Nimbo-<versión>.dmg (firma ad-hoc)
  DEVELOPER_ID="Developer ID Application: … (TEAMID)" NOTARY_PROFILE=rayneo scripts/make_dmg.sh
  ```
  - Sin Developer ID, en otros Macs Gatekeeper pide confirmar la primera vez (clic derecho > Abrir).
  - Con Developer ID (Apple Developer Program, 99 €/año) y `xcrun notarytool store-credentials rayneo`, la app se firma con *hardened runtime*, se notariza y se graba el ticket: abre sin avisos en cualquier Mac.
  - La versión sale de `project(... VERSION x.y.z)` en `CMakeLists.txt`.
- Nombre: **Nimbo** (propio; «RayNeo GT» solo se usa para nombrar las gafas compatibles). Identificador `io.github.davidsrc2.nimbo`. Al primer arranque se reutiliza la calibración de la versión anterior.

## Interfaz gráfica: requisitos (fase 5)

Hoy todo se controla con flags en Terminal. La fase 5 sustituye eso por una app de Mac. Este apartado es el brief de diseño.

**Tono: muy tranquila.** Es una herramienta que se usa con unas gafas puestas y la vista cansada.
- Pocos elementos, mucho aire, tipografía grande y legible (sistema, 15–17 pt de base).
- Colores suaves y apagados, sin rojos ni acentos estridentes. Modo oscuro por defecto.
- Animaciones lentas y discretas (≥ 250 ms, ease-in-out); nada que parpadee.
- Lenguaje llano en español, sin jerga técnica en la pantalla principal; lo técnico va en «Avanzado».

**Pantalla principal: disposición de monitores**
- Un lienzo con los monitores como tarjetas que se arrastran en una cuadrícula: al lado (horizontal) **o encima/debajo** (vertical), por ejemplo 2×1, 1×2, 2×2 o 3×1. Las tarjetas encajan con suavidad en la rejilla.
- Botones + / − para añadir o quitar monitores (1 a 6).
- Vista previa en perspectiva de cómo se verán alrededor de la cabeza (desde dentro, como en las gafas), que se actualiza en vivo.
- Plantillas de un toque: «Uno grande», «Dos lado a lado», «Dos apilados», «Tres en arco».

**Controles esenciales** (siempre visibles, como deslizadores grandes)
- **Tamaño del texto** (resolución de cada monitor virtual: 1024×576 … 1920×1080), con una etiqueta en lenguaje llano («Muy grande», «Grande», «Normal»).
- **Distancia** de los monitores (1,2–3 m).
- **Separación** entre monitores.
- **Plano / curvo**.

**Acciones**
- Un botón grande **Empezar / Parar**.
- **Recentrar**, con su atajo visible (⌘⌥C).
- Estado tranquilo en una línea: «Gafas conectadas · siguiendo tu cabeza» / «Calibrando, deja las gafas quietas…» / «Gafas desconectadas».

**Asistentes**
- **Primera vez:** conectar las gafas, conceder permisos (Grabación de pantalla, Accesibilidad) con explicación breve, calibrar mirando arriba, abajo, izquierda y derecha (con ilustración y progreso), y ajustar la escala girando la cabeza («si los monitores se mueven contigo, pulsa −»).
- **Recalibrar** accesible desde Ajustes.

**Avanzado** (plegado por defecto)
- FOV, IPD, mono/SBS (3D), zona muerta del estabilizador, cursor que sigue la mirada, traer ventanas al empezar, k1/k2.

**Comportamiento**
- Icono en la barra de menús (◎) con: Recentrar, Empezar/Parar, Abrir disposición, Salir.
- Recordar la última disposición y los ajustes.
- Al desconectar las gafas, parar y restaurar las pantallas, con un aviso discreto.

**Técnica (para quien implemente)**
- SwiftUI sobre el servidor actual (`apps/nimbo`): las opciones de `Options.swift` son el modelo, y la UI las edita y relanza el servidor (o las aplica en caliente cuando sea posible: FOV, distancia, separación, plano/curvo).
- La disposición vertical requiere ampliar `core::panel_centers` a una cuadrícula (yaw + pitch), mallas inclinadas en pitch y `VirtualDesktop` colocando los monitores en filas y columnas, para que el ratón pase entre ellos como en la realidad.

## Estado al 15-10-2026 y punto de continuación

- **Modo de la app (macOS y Windows):** «Espacio a tamaño real», el único en la app. Las gafas en follow (Nimbo las pone y las devuelve a pinned al salir); cada pantalla del tamaño de la imagen de las gafas, fija en la habitación en los dos ejes; estabilizador world-locked (`core::world_locked_stabilizer`), ancla en reposo opcional, inclinación, pantalla del ordenador como panel 1, ventanas flotantes separadas de los monitores. Pinned queda solo por línea de comandos (`--pinned`).
- **macOS:** probado con las gafas y estable; ⌘⌥R graba lo que muestran las gafas a Descargas.
- **Windows:** el servidor funciona con las gafas (120 fps, ~18 ms), con el Virtual Display Driver 25.7.23 y la DLL renombrada a `nimbo_server.dll`; la app WinUI ya abre. Lo que falta comprobar y hacer está en [docs/WINDOWS_PLAN.md](docs/WINDOWS_PLAN.md), sección «Estado al terminar el 15-10-2026».

## Windows

Plan de trabajo para la versión de Windows: [docs/WINDOWS_PLAN.md](docs/WINDOWS_PLAN.md).

## Capas

```
include/rayneo/
  core/      Protocolo GT + Madgwick + corrección de drift. C++17 puro, sin dependencias de SO.
  hal/       Transporte HID (hidapi) + hilo lector de alta frecuencia + ring SPSC lock-free.
  platform/  C ABI estable (rayneo_engine.h) para Swift / Obj-C++ / Win32.
  core/render_math.h  Mapeo Z-up RH -> Y-up LH, matrices, predicción, malla curva, paneles.
src/platform/macos/
  RNSpatialEngine.{h,mm}  Fachada Objective-C++ (ARC).
  module.modulemap        Importación directa del C ABI desde Swift.
  renderer/               RNSpatialRenderer (Metal) + Shaders.metal (compilado en runtime).
  virtual_display/        RNVirtualDisplay (monitores virtuales, API privada).
apps/imu_probe/           CLI: lista, volcado raw, calibración guiada, pose a 10 Hz.
apps/nimbo/               App Swift (Nimbo): captura, render espacial, hotkeys, cabeza simulada.
config/                   calibration.example.json
tests/                    Tests del núcleo (sin hardware).
```

Dependencias: `platform -> hal -> core`. `core` no enlaza nada.

Flujo de datos:

```
[hilo HID, QoS user-interactive]  hid_read_timeout -> SpscRing<RawReport,1024>
[hilo fusión]                     pop -> decode 99 65 -> GyroBiasEstimator -> Madgwick -> Pose
[cualquier hilo / CVDisplayLink]  rayneo_engine_latest_pose()
```

## Build (macOS, Apple Silicon)

```sh
brew install cmake ninja           # hidapi 0.15.0 se compila estático (-DRAYNEO_SYSTEM_HIDAPI=ON para usar el del sistema)
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
./build/imu_probe --list           # enumera dispositivos 0x3941
./build/imu_probe --raw 20         # descriptor, hex y cadencia de ticks sin pasar por el motor
./build/imu_probe --calibrate      # guiado: centro, arriba, abajo, izquierda, derecha -> calibration.json
./build/imu_probe                  # stream + pose (dejar las gafas quietas ~5 s); carga ./calibration.json
./build/imu_probe --calib f.json   # calibración explícita (falla si no es válida)
```

Solo núcleo (cualquier host, sin hidapi): `cmake -S . -B build -DRAYNEO_BUILD_HAL=OFF`.

## Escritorio espacial macOS (`Nimbo.app`)

Crea N monitores virtuales en macOS (por defecto 3), los captura con ScreenCaptureKit y los dibuja como monitores planos fijos en el espacio, orientados hacia ti y contiguos como en una mesa con varios monitores (`--curved` para paneles curvos). Mueves la cabeza para mirar de uno a otro; el ratón y las ventanas pasan entre ellos como entre monitores físicos.

- Monitores virtuales: API privada `CGVirtualDisplay` (la misma que DeskPad/BetterDisplay). Existen solo mientras corre el servidor; al salir, las ventanas que tuvieran vuelven a la pantalla principal.
- Disposición: el panel de la izquierda es tu pantalla principal real (`--no-main` para quitarlo); a su derecha, los monitores virtuales en el mismo orden en que macOS los coloca, así el ratón pasa de uno a otro como entre monitores físicos. Configuración de pantallas solo para la app, se revierte al salir.
- El cursor sigue a la mirada: al mirar otro panel salta a él conservando su posición relativa (`--no-gaze-cursor` para desactivarlo). `Cmd+Opt+C` también lo lleva al panel de delante.
- Panel por defecto: plano, 38° de ancho (cabe entero en el FOV horizontal de ~42°), separación 1°, a 2 m.
- Pose: IMU de las gafas con predicción hasta el instante de presentación. `--sim` sustituye la IMU por una cabeza simulada (sin gafas).
- `--displays 0` vuelve al modo de Fase 3 (captura de una pantalla existente).
- Latencia medida (M4, espejo, 120 Hz): muestra IMU -> scanout 26,4 ms = edad de pose 1,4 ms + 3 refrescos de WindowServer, compensada con predicción. `--unmirror`, 3 drawables, `frameLatency` < 1, sin vsync y modo exclusivo no lo reducen.

Atajos globales: `Cmd+Opt+C` recentrar · `Cmd+Opt+Q` salir · `Cmd+Opt+flechas` mirar alrededor (solo `--sim`). También en la barra de menús: **◎ Nimbo > Recentrar / Salir**.

- Estabilidad: `PoseStabilizer` con zona muerta. Mirando un punto, la imagen queda congelada como un monitor normal (0 temblor; tremor, pulso y respiración < 1,5° no llegan a la pantalla). Al girar de verdad sale de la zona muerta y sigue a la cabeza con un filtro rápido (< 0,6° de retraso a 60°/s) hasta que vuelves a quedarte quieto. La predicción solo se aplica al girar.
- Escritorio virtual: por defecto todos los paneles son monitores virtuales; el panel 1 es la pantalla principal de macOS (barra de menús, Dock, ventanas nuevas) y la pantalla física de las gafas queda en una esquina (solo superficie de dibujo). Un solo cursor, siempre dentro de los paneles. Las ventanas abiertas se traen al panel 1 si Terminal tiene permiso de Accesibilidad. `--keep-main` vuelve al modo anterior (la pantalla de las gafas como panel izquierdo; dos cursores).
- Nitidez: los paneles se muestrean con mipmaps y filtrado anisotrópico, así el texto reducido no vibra ni se ve dentado con micromovimientos.
- Color: pipeline sRGB (filtrado, mipmaps y atenuado en luz lineal), sin bordes de texto oscurecidos.
- Ahorro de energía: con la cabeza quieta y sin cambios en ningún monitor no se redibuja (el log muestra `render N fps (M idle)`).
- Texto: monitores virtuales de 1152x648 puntos (HiDPI, captura a 2x): todo 1,67x más grande que a 1920x1080. `--vd-size` para cambiarlo.

### Prueba sin gafas

```sh
./build/Nimbo.app/Contents/MacOS/Nimbo --windowed --sim --test-pattern
```

Ventana de vista previa en la pantalla del Mac con los 3 paneles (patrón de color con etiqueta y reloj). `Cmd+Opt+flechas` para girar la "cabeza", `--sim-sweep 50` para un barrido automático.

### Prueba completa con las gafas (de cero)

0. Requisitos (una vez)
   - Compilar: `cd ~/rayneo-gt-spatial-core && cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build`
   - Permiso de grabación de pantalla para Terminal (Ajustes > Privacidad y seguridad > Grabación de pantalla y audio del sistema). Si se concede por primera vez, cerrar y reabrir Terminal.
   - Gafas en su modo normal (imagen pegada a la vista), **sin** ningún modo de pantalla fija propio. Modo 3D (SBS) para el render estéreo; si están en 2D usar `--mono` (sin él se ve la misma pantalla duplicada, una mitad por ojo).

1. Conectar las gafas y comprobar el enlace USB
   ```sh
   ./build/imu_probe --list        # debe listar 3941:af50 "RayNeo AR Glasses"
   ./build/imu_probe --raw 5       # "device-clock rate ~457 Hz, missed 0", "stream-on ACK: yes"
   ```

2. Calibrar el montaje sensor -> cabeza (una vez por persona/unidad)
   ```sh
   ./build/imu_probe --calibrate
   ```
   - `[0/5]` gafas quietas **sobre la mesa** hasta `bias ok` (~6 s).
   - `[1/5]` ponérselas, mirar al frente con la cabeza recta y quedarse quieto.
   - `[2/5]`..`[5/5]` mirar arriba / abajo / girar izquierda / derecha (~30°), mantener, volver al centro, mantener. La línea de estado indica `move`, `further` (gira más) u `hold`.
   - Resultado: `calibration.json` en la carpeta del repo.

3. Verificar la calibración
   ```sh
   ./build/imu_probe               # debe decir "calibration: calibration.json"
   ```
   Gafas en la mesa hasta `TRACK`, ponérselas: mirar arriba cambia **pitch**, girar a la izquierda cambia **yaw**, inclinar hacia un hombro cambia **roll**. Ctrl+C.

4. Lanzar el escritorio espacial (desde la carpeta del repo, para que cargue `calibration.json`)
   ```sh
   ./build/Nimbo.app/Contents/MacOS/Nimbo --test-pattern
   ```
   - Gafas en la mesa hasta que el log diga `TRACK` (~6 s), ponérselas.
   - Mirar al frente y pulsar `Cmd+Opt+C` (recentrar): el panel 2 (verde) queda delante, el 1 (azul) a la izquierda y el 3 (marrón) a la derecha.

5. Comprobar el anclaje
   - Girar la cabeza despacio a izquierda y derecha: los paneles no se mueven respecto al mundo.
   - Movimientos rápidos y paradas en seco: sin retraso apreciable ni efecto gelatina/rebote.
   - Quedarse 1-2 min mirando el panel central: no debe desplazarse solo (drift).
   - Si se ve doble: `--ipd 60` (o tu distancia interpupilar). Si los bordes se curvan: probar `--k1 0.05`.

6. Trabajar
   - Relanzar sin `--test-pattern`. Con `--displays 1` se ven dos monitores: tu pantalla del Mac (izquierda) y uno virtual (derecha).
   - Mira el panel que quieras: el cursor salta a él. Arrastra ventanas desde tu pantalla (izquierda) a los monitores virtuales, o botón verde de la ventana > Mover a "RayNeo Panel N".
   - Ajustes útiles: `--displays 2`, `--arc 34` (paneles más pequeños), `--distance 1.6`, `--hidpi` (texto más nítido, más coste de captura).
   - `--fit`: todos los paneles caben a la vez en la vista. Las gafas tienen 1920 px de ancho por ojo, así que con 2 paneles cada uno se ve con ~960 px (texto pequeño); sin `--fit` cada panel se ve a resolución completa y se gira la cabeza para pasar de uno a otro.
   - Salir: `Cmd+Opt+Q` o Ctrl+C. Los monitores virtuales desaparecen y las ventanas vuelven a la pantalla principal.

7. Qué reportar: anclaje (sí/no), drift tras 2 min, sensación de latencia, nitidez/doble imagen, si el SBS se ve en 3D correctamente, y el log de la terminal (líneas `TRACK render ...`).

## Calibración sensor -> cabeza

`calibration.json` (ver `config/calibration.example.json`) guarda el cuaternión `sensor_to_head` (`v_head = q·v_sensor·q⁻¹`, cabeza X adelante, Y izquierda, Z arriba). Arriba = gravedad en el centro; izquierda = eje del cabeceo (abajo − arriba) ortogonalizado; los giros izquierda/derecha solo validan (eje de yaw vs gravedad ≤20°). Es específico de cada unidad y usuario: no se versiona.

## Protocolo

Ver [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Créditos

Protocolo y parámetros de fusión derivados de [Rayneo-Spatial-App](https://github.com/AkshayReddyGujjula/Rayneo-Spatial-App) (Apache-2.0).
