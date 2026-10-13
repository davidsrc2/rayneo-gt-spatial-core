# Prompt para Gemini — continuación de rayneo-gt-spatial-core

Copia todo lo que hay debajo de la línea y pégalo en Gemini.

---

Actúa como arquitecto de software de sistemas experto en C++17 multiplataforma, IOKit/hidapi en macOS, fusión de sensores IMU en tiempo real y gestores de ventanas espaciales para gafas AR (tipo RealWarp / XREAL Nebula).

Tu tarea NO es escribir código. Tu tarea es generar un **prompt detallado y ejecutable** que yo pasaré a un agente de programación (Claude Code, con acceso a terminal, git y gh) para que continúe el proyecto descrito abajo. El prompt debe estar ordenado por fases, con criterios de aceptación verificables en cada fase.

## Contexto del proyecto

- Objetivo: PoC de gestor de ventanas espaciales 3DoF para las gafas **RayNeo GT** en un **Mac Apple Silicon (M4)**. Largo plazo: el núcleo debe compilar también en **Windows**.
- RayNeo no tiene SDK para este modelo. El protocolo USB HID está descifrado en el repo público `AkshayReddyGujjula/Rayneo-Spatial-App` (Windows, Apache-2.0).
- Repo propio (privado): `github.com/davidsrc2/rayneo-gt-spatial-core`, rama `main`, commit inicial `7aa10c9`.

## Protocolo RayNeo GT (verificado en Windows por el repo de referencia, NO en macOS)

- VID:PID runtime `3941:AF50` (DFU `3941:AF51`). Interfaz vendor HID, usage page `0xFF00`, usage `1`. Board id `0x40` GT, `0x41` GT Max.
- Comandos: output report de 64 B + report id: `[0x00][0x66][cmd][0x00...]`. `cmd`: `00` device info, `01` stream on, `02` stream off (el stream persiste tras cerrar el proceso), `3c`/`3e` sin respuesta en este firmware.
- ACK: `99 c8`, opcode reflejado en byte 8.
- Informe IMU `99 65` (64 B, LE): accel float32 x3 @4 (m/s²), gyro float32 x3 @16 (deg/s), temp @28 (°C), mag X/Y @32/@36, tick uint32 @40 (100 µs, +21/muestra ≈ 476 Hz), mag Z @52 (µT).
- Notas de fusión del repo de referencia: magnetómetro apagado por defecto (interferencia → giro de yaw), primeros 4 s tras stream-on contaminados, bias de giroscopio estimado en runtime (no hay bias de fábrica), rotación relativa `q_rel = q * q_ref⁻¹` (marco tierra, Z arriba), mapeo sensor→cabeza por calibración medida (no hardcodeado). Marco render en su app: Y arriba, LH (Direct3D).

## Lo que ya está hecho

Arquitectura en tres capas, dependencias `platform -> hal -> core`; `core` no enlaza nada.

```
include/rayneo/core/      types.h, gt_protocol.h, madgwick.h, gyro_bias_estimator.h, imu_pipeline.h
include/rayneo/hal/       hid_transport.h, spsc_ring.h, imu_stream_reader.h
include/rayneo/platform/  rayneo_engine.h  (C ABI estable)
src/core/                 gt_protocol.cpp, madgwick.cpp, gyro_bias_estimator.cpp, imu_pipeline.cpp
src/hal/                  hidapi_transport.cpp, imu_stream_reader.cpp
src/platform/             rayneo_engine.cpp
src/platform/macos/       RNSpatialEngine.h/.mm (fachada Obj-C++, ARC), module.modulemap (Swift)
apps/imu_probe/main.cpp   CLI: --list enumera; sin args imprime yaw/pitch/roll, bias y Hz a 10 Hz
tests/core_tests.cpp      9 tests sin hardware
docs/PROTOCOL.md
CMakeLists.txt
```

Detalle:
- **core**: decoder de `99 65` y `99 c8` (acepta con y sin report id inicial, rechaza NaN/longitud corta); `build_command`; delta de tick con desbordamiento seguro. `MadgwickFilter` IMU y MARG, gyro en rad/s, `initialize_from_accel`. `GyroBiasEstimator`: warmup 4 s/150 muestras → ventana de reposo contigua ≥1 s/600 muestras → tracking; detector de reposo por EMA (entrada gyro <1.5 deg/s y accel <0.5 m/s², salida 3.0/1.0, dwell 0.5 s, hold-off 1 s); bias limitado a ±1.5 deg/s; adaptación en reposo solo si residuo ≤0.35 deg/s; gap ≥50 ms invalida ventana y dwell. `ImuPipeline`: bytes HID → `Pose` (absolute, relative, euler diagnóstico, bias, tick, tracking), recentrado.
- **hal**: `HidTransport` abstracto + implementación hidapi (`hid_darwin_set_open_exclusive(0)` en macOS). `ImuStreamReader`: localiza las gafas por VID/PID + usage page, envía stream on y espera ACK, hilo lector con `QOS_CLASS_USER_INTERACTIVE` (Windows: `THREAD_PRIORITY_TIME_CRITICAL`) que vuelca a un `SpscRing<RawReport,1024>` lock-free con contador de descartes; al parar envía stream off.
- **platform**: `rayneo_engine` C ABI (create/start/stop/latest_pose/recenter/last_error); hilo de fusión que vacía el ring y publica la última pose bajo mutex; `latest_pose` llamable desde cualquier hilo (p. ej. CVDisplayLink). Fachada `RNSpatialEngine`/`RNPose` con `simd_quatf`.
- **CMake**: C++17, arm64 y macOS 13.0 por defecto, OBJCXX en Apple. hidapi: `find_package` → pkg-config → FetchContent `hidapi-0.14.0`. Enlaza IOKit, CoreFoundation, Foundation. Opciones `RAYNEO_BUILD_HAL`, `RAYNEO_BUILD_TESTS`, `RAYNEO_BUILD_APPS`, `RAYNEO_FETCH_HIDAPI`.

## Estado de verificación

- Verificado: núcleo + tests compilados con g++ en Linux, 9/9 pasan (decoder, wrap de tick, Madgwick estable/inclinación/integración de yaw, eliminación de bias constante, recentrado).
- NO verificado: HAL, engine, CMake completo, Obj-C++ y hardware real. Nada se ha ejecutado en macOS ni con las gafas conectadas.

## Pendiente conocido

1. Validar en el M4: build con CMake/Ninja, `imu_probe --list`, tasa real ~476 Hz, si hidapi en macOS devuelve report id inicial, si macOS requiere permiso de Input Monitoring / entitlement para abrir el HID.
2. Mapeo de ejes sensor → cabeza (calibración guiada que mida la rotación propia y la persista en JSON); hoy la pose está en marco del sensor.
3. Ruta de escape/rollback del estimador de bias para escalones persistentes (TODO en `gyro_bias_estimator.cpp`).
4. Conversión marco tierra (Z arriba, RH) → marco render de Metal/SceneKit/RealityKit.
5. Capa OS macOS: captura de pantalla/ventanas (ScreenCaptureKit), render en la pantalla de las gafas (las GT aparecen como monitor externo), cámara anclada al mundo con la pose, recentrado por atajo global, latencia motion-to-photon y predicción de pose.
6. Robustez: reconexión en hot-plug, logging/CSV de sesiones, replay de capturas para tests.
7. Portabilidad Windows: CI (GitHub Actions) en macOS + Windows + Linux para el núcleo, backend Win32 de la capa OS.

## Lo que necesito que me devuelvas

Un único prompt, en español, dirigido a Claude Code, que:
- Asuma el repo clonado en `~/rayneo-gt-spatial-core` en macOS (Apple Silicon) y que `gh` está autenticado.
- Esté dividido en fases priorizadas (primero validación en hardware, luego calibración de ejes y robustez del bias, luego capa OS de macOS, luego CI/Windows).
- Para cada fase: objetivo, archivos a crear/modificar respetando la separación `core`/`hal`/`platform`, decisiones técnicas concretas (APIs de macOS a usar, estructuras de datos, hilos), tests y criterios de aceptación medibles (Hz, deriva en deg/min, latencia en ms).
- Exija que `core` siga sin dependencias de SO y que toda funcionalidad nueva de `core` tenga tests sin hardware.
- Pida un commit por fase con mensaje descriptivo y push a `main` (o rama + PR si lo consideras mejor; justifícalo).
- Indique explícitamente qué debe verificarse con las gafas conectadas y qué debe reportar el agente si algo no coincide con el protocolo documentado.
- No explique conceptos básicos de programación.
