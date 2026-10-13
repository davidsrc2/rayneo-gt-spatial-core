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

## Calibración sensor -> cabeza

`calibration.json` (ver `config/calibration.example.json`) guarda el cuaternión `sensor_to_head` (`v_head = q·v_sensor·q⁻¹`, cabeza X adelante, Y izquierda, Z arriba). Arriba = gravedad en el centro; izquierda = eje del cabeceo (abajo − arriba) ortogonalizado; los giros izquierda/derecha solo validan (eje de yaw vs gravedad ≤20°). Es específico de cada unidad y usuario: no se versiona.

## Protocolo

Ver [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Créditos

Protocolo y parámetros de fusión derivados de [Rayneo-Spatial-App](https://github.com/AkshayReddyGujjula/Rayneo-Spatial-App) (Apache-2.0).
