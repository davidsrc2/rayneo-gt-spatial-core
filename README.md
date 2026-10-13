# rayneo-gt-spatial-core

Motor 3DoF multiplataforma para las gafas RayNeo GT (USB HID, ~476 Hz). PoC para un gestor de ventanas espaciales en macOS (Apple Silicon); el núcleo es portable a Windows.

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
brew install cmake hidapi          # hidapi opcional: si falta se descarga 0.14.0
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
./build/imu_probe --list           # enumera dispositivos 0x3941
./build/imu_probe                  # stream + pose (dejar las gafas quietas ~5 s)
```

Solo núcleo (cualquier host, sin hidapi): `cmake -S . -B build -DRAYNEO_BUILD_HAL=OFF`.

## Protocolo

Ver [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Créditos

Protocolo y parámetros de fusión derivados de [Rayneo-Spatial-App](https://github.com/AkshayReddyGujjula/Rayneo-Spatial-App) (Apache-2.0).
