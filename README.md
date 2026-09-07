# Faital Speaker Bluetooth prototype

ESP32 Bluetooth Classic A2DP Sink firmware for this tested audio path:

`ESP32 -> I2S -> PCM5102A -> AUX -> TPA3116D2 -> two FaitalPRO speakers`

## I2S wiring

| ESP32 | PCM5102A |
| --- | --- |
| GPIO26 | BCK |
| GPIO25 | LCK/LRCK/WS |
| GPIO22 | DIN |
| GND | GND |
| 5V | VCC |

The Bluetooth device name is `Faital Speaker Proto`.

## Build and flash

1. Turn off the 12 V adapter.
2. Disconnect `OUT+` of LM2596 from the breadboard 5 V rail.
3. Keep the amplifier volume at minimum.
4. Connect ESP32 to the computer by USB.
5. Delete the old generated `sdkconfig.esp32doit-devkit-v1` file if it exists.
6. Run PlatformIO **Clean**.
7. Run PlatformIO **Build**.
8. Run PlatformIO **Upload**.
9. Open PlatformIO **Monitor** at 115200 baud.

Expected startup log:

```text
I2S ready: BCK=26, LRCK=25, DATA=22, rate=44100 Hz
Bluetooth ready: Faital Speaker Proto
```

After a successful upload, disconnect USB, reconnect `OUT+` of LM2596 to the
5 V rail, then apply 12 V power. Find `Faital Speaker Proto` on the phone and
pair with it. If a legacy PIN is requested, use `1234`.

Start playback with phone volume at 10-20%, then slowly raise the amplifier
volume. The firmware stores the last connected Bluetooth address and makes one
reconnection attempt after the next power-up.

