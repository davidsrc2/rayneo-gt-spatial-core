# rayneo-gt-spatial-core

Motor 3DoF multiplataforma para las gafas RayNeo GT (USB HID, ~457 Hz). PoC para un gestor de ventanas espaciales en macOS (Apple Silicon); el núcleo es portable a Windows.

## Capas

```
include/rayneo/
  core/      Protocolo GT + Madgwick + corrección de drift. C++17 puro, sin dependencias de SO.
  hal/       Transporte HID (hidapi) + hilo lector de alta frecuencia + ring SPSC lock-free.
  platform/  C ABI estable (rayneo_engine.h) para Swift / Obj-C++ / Win32.
src/platform/macos/
  RNSpatialEngine.{h,mm}  Fachada Objective-C++ (ARC).
  module.modulemap        Importación directa del C ABI desde Swift.
apps/imu_probe/           CLI: descubre el dispositivo e imprime la pose.
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

Crea N monitores virtuales en macOS (por defecto 3), los captura con ScreenCaptureKit y los dibuja como paneles curvos fijos en el espacio alrededor de ti (izquierda, centro, derecha). Mueves la cabeza para mirar de uno a otro; el ratón y las ventanas pasan entre ellos como entre monitores físicos.

- Monitores virtuales: API privada `CGVirtualDisplay` (la misma que DeskPad/BetterDisplay). Existen solo mientras corre el servidor; al salir, las ventanas que tuvieran vuelven a la pantalla principal.
- Disposición en macOS: fila a la derecha de la pantalla principal (panel 1 = izquierda). Configuración de pantallas solo para la app, se revierte al salir.
- Panel por defecto: 38° de ancho (cabe entero en el FOV horizontal de ~42°), separación 4°, radio 2 m.
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
   - Gafas en su modo normal (imagen pegada a la vista), **sin** ningún modo de pantalla fija propio. Modo 3D (SBS) para el render estéreo; si se quedan en 2D usar `--mono`.

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
   - Relanzar sin `--test-pattern`. Mover el ratón hacia la derecha desde la pantalla principal entra en el panel 1, luego 2 y 3.
   - Arrastrar ventanas a los paneles (o usar Mission Control / Rectangle). El cursor se ve dentro de los paneles.
   - Ajustes útiles: `--displays 2`, `--arc 34` (paneles más pequeños), `--distance 1.6`, `--hidpi` (texto más nítido, más coste de captura).
   - Salir: `Cmd+Opt+Q` o Ctrl+C. Los monitores virtuales desaparecen y las ventanas vuelven a la pantalla principal.

7. Qué reportar: anclaje (sí/no), drift tras 2 min, sensación de latencia, nitidez/doble imagen, si el SBS se ve en 3D correctamente, y el log de la terminal (líneas `TRACK render ...`).

## Calibración sensor -> cabeza

`calibration.json` (ver `config/calibration.example.json`) guarda el cuaternión `sensor_to_head` (`v_head = q·v_sensor·q⁻¹`, cabeza X adelante, Y izquierda, Z arriba). Arriba = gravedad en el centro; izquierda = eje del cabeceo (abajo − arriba) ortogonalizado; los giros izquierda/derecha solo validan (eje de yaw vs gravedad ≤20°). Es específico de cada unidad y usuario: no se versiona.

## Protocolo

Ver [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Créditos

Protocolo y parámetros de fusión derivados de [Rayneo-Spatial-App](https://github.com/AkshayReddyGujjula/Rayneo-Spatial-App) (Apache-2.0).
