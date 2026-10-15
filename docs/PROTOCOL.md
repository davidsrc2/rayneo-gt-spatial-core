# RayNeo GT — protocolo USB HID

Fuente: `docs/PROTOCOL-NOTES.md` de Rayneo-Spatial-App (verificado en Windows 11, firmware "Sep 7 2026"). Verificado en macOS 27.2 / M4 contra la unidad de referencia (ver abajo); donde difieren, manda la unidad de referencia.

## Identidad USB

| Campo | Valor |
|---|---|
| VID:PID runtime | `3941:AF50` |
| VID:PID DFU | `3941:AF51` |
| Interfaz | vendor HID, usage page `0xFF00`, usage `1` |
| Board id | `0x40` GT, `0x41` GT Max |

## Comandos (output report, 64 B + report id)

`[0x00 report id][0x66][cmd][valor][0x00 ...]`

| cmd | Significado |
|---|---|
| `00` | device info |
| `01` | stream on |
| `02` | stream off (el stream persiste tras cerrar el proceso) |
| `08` | modo de imagen: valor `00` follow, `01` pinned, `02` steady. Cambiar de modo recentra el ancla de pinned (medido 2026-10-15, firmware "Sep 22 2026") |
| `09` | brillo: valor = nivel (`08` el de fábrica medido; `00` casi apagado). El device info lo refleja en el byte 41 |
| `3c` / `3e` | bias de fábrica / tabla térmica — sin respuesta en este firmware |

El botón de modo de las gafas envía por su cuenta un `99 c8` con `08` en el byte 8 (sin el modo elegido). `07` no tiene efecto visible.

ACK: `99 c8`, opcode reflejado en byte 8.

## Informe IMU `99 65` (64 B, little-endian)

| Offset | Contenido |
|---|---|
| 4 | accel X,Y,Z float32 (m/s²) |
| 16 | gyro X,Y,Z float32 (deg/s) |
| 28 | temperatura float32 (°C) |
| 32 / 36 | mag X / Y float32 (µT) |
| 40 | tick uint32 (100 µs) — +21.875 por muestra (patrón 22×7, 21×1) ≈ 457.14 Hz |
| 48 | copia del tick (no usada) |
| 52 | mag Z float32 (µT) |

## Notas de fusión

- Magnetómetro desactivado por defecto: la interferencia del escritorio induce giro de yaw constante.
- Primeros 4 s tras `66 01` contaminados: warmup, luego una única ventana de reposo contigua ≥1 s / ≥600 muestras para el bias.
- Bias limitado a ±1.5 deg/s; adaptación en reposo solo para residuos ≤0.35 deg/s.
- Rotación relativa: `q_rel = q * q_ref⁻¹` (marco tierra). `q_ref⁻¹ * q` filtra yaw a pitch/roll si se recentra con la cabeza inclinada.
- Gap de tick ≥50 ms invalida la ventana de calibración y el dwell de reposo.

- Escalón de bias fuera de la ventana rutinaria (>0.35 deg/s, ≤1.5 deg/s): se adopta solo tras 10 s de reposo continuo con la EMA estable (±0.15 deg/s). Durante 30 s posteriores, si el giroscopio vuelve al bias anterior durante 2 s, se hace rollback (era un giro lento, no deriva).
- Montaje sensor -> cabeza: `calibration.json` (`imu_probe --calibrate`). Solo define el rumbo "al frente"; la pose relativa es el delta en marco tierra expresado en el marco nivelado alineado con ese rumbo.

## Verificación macOS (unidad de referencia, 2026-10-13)

Medido con `imu_probe --raw 20`:

- Report descriptor (40 B): page `0xFF00`, input y output de 64 B, **sin** item Report ID. hidapi/IOKit entrega 64 B sin byte de report id; `hid_write` acepta 65 B con `0x00` inicial.
- Dos colecciones (`ff00:0001`, `ff00:0002`) en el mismo `IOHIDDevice`; ambas enumeran la misma ruta.
- ACK `99 c8` a ~1 ms del `66 01`; bytes 4..7 del ACK = tick del dispositivo, byte 8 = opcode.
- Deltas de tick en 20 s: 22 → 8022, 21 → 1118, 23 → 1. Media 21.875 ticks = **457.14 Hz** (no 476). Sin pérdidas ni huecos.
- Input Monitoring: no requerido para la página vendor `0xFF00`.
