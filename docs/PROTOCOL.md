# RayNeo GT — protocolo USB HID

Fuente: `docs/PROTOCOL-NOTES.md` de Rayneo-Spatial-App (verificado en Windows 11, firmware "Sep 7 2026"). Pendiente de verificar en macOS.

## Identidad USB

| Campo | Valor |
|---|---|
| VID:PID runtime | `3941:AF50` |
| VID:PID DFU | `3941:AF51` |
| Interfaz | vendor HID, usage page `0xFF00`, usage `1` |
| Board id | `0x40` GT, `0x41` GT Max |

## Comandos (output report, 64 B + report id)

`[0x00 report id][0x66][cmd][0x00 ...]`

| cmd | Significado |
|---|---|
| `00` | device info |
| `01` | stream on |
| `02` | stream off (el stream persiste tras cerrar el proceso) |
| `3c` / `3e` | bias de fábrica / tabla térmica — sin respuesta en este firmware |

ACK: `99 c8`, opcode reflejado en byte 8.

## Informe IMU `99 65` (64 B, little-endian)

| Offset | Contenido |
|---|---|
| 4 | accel X,Y,Z float32 (m/s²) |
| 16 | gyro X,Y,Z float32 (deg/s) |
| 28 | temperatura float32 (°C) |
| 32 / 36 | mag X / Y float32 (µT) |
| 40 | tick uint32 (100 µs) — +21 por muestra ≈ 476 Hz |
| 52 | mag Z float32 (µT) |

## Notas de fusión

- Magnetómetro desactivado por defecto: la interferencia del escritorio induce giro de yaw constante.
- Primeros 4 s tras `66 01` contaminados: warmup, luego una única ventana de reposo contigua ≥1 s / ≥600 muestras para el bias.
- Bias limitado a ±1.5 deg/s; adaptación en reposo solo para residuos ≤0.35 deg/s.
- Rotación relativa: `q_rel = q * q_ref⁻¹` (marco tierra). `q_ref⁻¹ * q` filtra yaw a pitch/roll si se recentra con la cabeza inclinada.
- Gap de tick ≥50 ms invalida la ventana de calibración y el dwell de reposo.

## Pendiente

- Mapeo de ejes sensor -> cabeza (calibración de orientación).
- Ruta de escape/rollback del estimador de bias para escalones persistentes.
- Verificar en macOS si hidapi devuelve el report id inicial (el decoder acepta ambos casos).
