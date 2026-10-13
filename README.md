# rayneo-gt-spatial-core

Motor 3DoF multiplataforma para las gafas RayNeo GT (USB HID, ~457 Hz). PoC para un gestor de ventanas espaciales en macOS (Apple Silicon); el núcleo es portable a Windows.

## Estado del proyecto

Última actualización: 2026-10-13. Hardware de referencia: RayNeo GT conectadas a un MacBook M4 (macOS 27.2). Cuando el hardware contradice la documentación, manda el hardware.

| Fase | Contenido | Estado |
|---|---|---|
| 1 | HAL en macOS M4: build arm64 sin warnings, hidapi estático, `imu_probe --list/--raw`, `TickMonitor`, stats en el C ABI | Fusionada (PR #1). Validada con las gafas |
| 2 | Escape y rollback del bias, calibración `sensor_to_head` (`imu_probe --calibrate`, `calibration.json`) | Fusionada (PR #2). Tests sin hardware |
| fix | Calibración guiada atascada con las gafas puestas: bias sobre la mesa + `HoldDetector` | Fusionada (PR #3). Pendiente de probar con las gafas puestas |
| 3 | Servidor Metal: ScreenCaptureKit, pantalla curva, half-SBS, predicción, `Cmd+Opt+C` | Rama `feat/phase3-macos-renderer`. Funciona sobre las gafas (120 fps); falta validación visual |
| 3b | Escritorio de N monitores virtuales (`CGVirtualDisplay`) como paneles fijos, cabeza simulada `--sim` | Rama `feat/phase3b-virtual-displays` (encima de la 3). Probado sin gafas; falta prueba con las gafas |
| 4 | CI en GitHub Actions: macOS arm64, Windows MSVC, Linux GCC/Clang + chequeo de aislamiento de core/hal | Fusionada (PR #4). Verde en los 3 SO, 0 warnings propios en MSVC |

### Medidas sobre el hardware de referencia

- **Informe IMU:** 64 B sin report ID. El ACK `99 c8` llega en ~1 ms.
- **Frecuencia real del sensor:** **457,14 Hz**, no los 476 documentados. Los deltas de tick son 22 siete de cada ocho veces y 21 la restante.
- **Estabilidad del stream:** 0 pérdidas y 0 drops del ring en sesiones de 20 s. El bias en reposo es de unos 0,4 °/s.
- **Pantalla `SmartGlasses`:** 1920×1080 a 60 o 120 Hz, en espejo con la pantalla interna. No hay modo 3840×1080, así que el estéreo es half-SBS.
- **Latencia muestra IMU → scanout:** 26,4 ms con espejo a 120 Hz, que es el suelo de WindowServer (1,4 ms de edad de la pose más 3 refrescos). La predicción la compensa.
- **Ajustes de latencia probados sin efecto:** quitar el espejo (43 ms), modo exclusivo (43 ms), 3 drawables y `frameLatency` < 1 dan lo mismo; sin vsync el scanout no se adelanta.

### Próximos pasos

1. Probar con las gafas la calibración y el escritorio de 3 paneles, siguiendo la guía de más abajo.
2. Si la prueba sale bien, fusionar las fases 3 y 3b.
3. Backend Windows (Win32/Direct3D) sobre el mismo core; el CI ya compila core y hal en MSVC.

### Riesgos conocidos

- `CGVirtualDisplay` es una API privada: puede romperse con una actualización de macOS y la app no pasaría la revisión de la App Store.
- La distorsión de lente está desactivada (`k1 = k2 = 0`) porque no se conoce la óptica de las GT.
- El FOV de 24,5° es una estimación pendiente de ajustar con las gafas.

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
apps/mac_spatial_server/  App Swift: captura, render espacial, hotkeys, cabeza simulada.
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

## Escritorio espacial macOS (`mac_spatial_server.app`)

Crea N monitores virtuales en macOS (por defecto 3), los captura con ScreenCaptureKit y los dibuja como monitores planos fijos en el espacio, orientados hacia ti y contiguos como en una mesa con varios monitores (`--curved` para paneles curvos). Mueves la cabeza para mirar de uno a otro; el ratón y las ventanas pasan entre ellos como entre monitores físicos.

- Monitores virtuales: API privada `CGVirtualDisplay` (la misma que DeskPad/BetterDisplay). Existen solo mientras corre el servidor; al salir, las ventanas que tuvieran vuelven a la pantalla principal.
- Disposición: el panel de la izquierda es tu pantalla principal real (`--no-main` para quitarlo); a su derecha, los monitores virtuales en el mismo orden en que macOS los coloca, así el ratón pasa de uno a otro como entre monitores físicos. Configuración de pantallas solo para la app, se revierte al salir.
- El cursor sigue a la mirada: al mirar otro panel salta a él conservando su posición relativa (`--no-gaze-cursor` para desactivarlo). `Cmd+Opt+C` también lo lleva al panel de delante.
- Panel por defecto: plano, 38° de ancho (cabe entero en el FOV horizontal de ~42°), separación 1°, a 2 m.
- Pose: IMU de las gafas con predicción hasta el instante de presentación. `--sim` sustituye la IMU por una cabeza simulada (sin gafas).
- `--displays 0` vuelve al modo de Fase 3 (captura de una pantalla existente).
- Latencia medida (M4, espejo, 120 Hz): muestra IMU -> scanout 26,4 ms = edad de pose 1,4 ms + 3 refrescos de WindowServer, compensada con predicción. `--unmirror`, 3 drawables, `frameLatency` < 1, sin vsync y modo exclusivo no lo reducen.

Atajos globales: `Cmd+Opt+C` recentrar · `Cmd+Opt+Q` salir · `Cmd+Opt+flechas` mirar alrededor (solo `--sim`).

### Prueba sin gafas

```sh
./build/mac_spatial_server.app/Contents/MacOS/mac_spatial_server --windowed --sim --test-pattern
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
   ./build/mac_spatial_server.app/Contents/MacOS/mac_spatial_server --test-pattern
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
