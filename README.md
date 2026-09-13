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

The latest hardware test supplied by the user produced a clean standalone tone
at 176399–176400 B/s, while Bluetooth playback showed incomplete HCI reassembly
and low PCM input rates. See the [Bluetooth/HCI investigation](docs/bluetooth-investigation.md)
for installed-source evidence, diagnostic counters, normal/verbose/minimal tests,
and separate ESP-IDF 6.1.0 / 6.0.1 build environments. The underlying transport
cause still needs the controlled board tests described there.

## Build and flash

1. Turn off the 12 V adapter.
2. Disconnect `OUT+` of LM2596 from the breadboard 5 V rail.
3. Keep the amplifier volume at minimum.
4. Connect ESP32 to the computer by USB.
5. Keep the existing `sdkconfig.esp32doit-devkit-v1` configuration.
6. Run PlatformIO **Build** (`pio run`).
7. Run PlatformIO **Upload** (`pio run -t upload`) when ready to flash.
8. Open PlatformIO **Monitor** (`pio device monitor -b 115200`).

Expected startup log:

```text
I2S init: port=0, role=master, BCK=GPIO26, LRCK/WS=GPIO25, DATA=GPIO22
I2S port 0 initialized and enabled
Bluetooth device name: Faital Speaker Proto
Bluetooth ready: connectable=yes, discoverable=general
```

After a successful upload, disconnect USB, reconnect `OUT+` of LM2596 to the
5 V rail, then apply 12 V power. Find `Faital Speaker Proto` on the phone and
pair with it. If a legacy PIN is requested, use `1234`.

Start playback with phone volume at 10-20%, then slowly raise the amplifier
volume. The firmware stores the last connected Bluetooth address and makes one
reconnection attempt after the next power-up.

## Audio investigation and changes

The entry point is `src/main.c`; audio processing lives in
`src/audio_pipeline.c` and `src/i2s_output.c`. The inspected writer did **not**
contain a delay after successful I2S writes. Its delays were only in the stopped
and prefetch branches. The I2S API receives a 1000 **millisecond** timeout and
paces writes by waiting for DMA buffers; the timeout is not a per-write sleep.
Each DMA descriptor contains 960 stereo frames (3840 bytes), below the ESP32
4092-byte aligned limit. Three descriptors provide about 65.3 ms at 44100 Hz.
The 4096-byte application block can span descriptors; it need not equal their size.

Two concrete loss mechanisms were present:

- `audio_writer_task` consumed a block, called I2S once, and abandoned an unwritten
  suffix on a short write. `audio_write_all` now retains that block and retries its
  remaining whole frames, including after a timeout that wrote a prefix. Successful
  writes have no extra delay. A zero-progress retry yields one tick to avoid a busy
  loop. Suspension/disconnection discards the pending suffix intentionally and
  counts it in `discarded`. An impossible non-frame-aligned driver result halts
  the writer visibly instead of continuing with a corrupt byte offset.
- Latency trimming started above 16 KiB in a 32 KiB StreamBuffer. It now triggers
  only at 28 KiB or higher and drains whole frames back to the existing 8 KiB
  target. The buffer itself was not enlarged. This is emergency recovery, **not
  a fix for a sustained producer/consumer rate mismatch**.

The supplied counters give this exact accounting:

```text
9895936 received - 9887744 read = 8192 buffered
9887744 read - 8704000 written = 1179648 trimmed + 4096 in flight
```

This proves deliberate PCM removal, which can introduce audible discontinuities.
It does not prove why the consumer fell behind or that trimming alone explains
the reported digital noise. In that log `short=0`, so the short-write defect was
not active. The single SBC decoding failure is not established as the cause.
At 44100 Hz, s16 stereo requires 176400 B/s; 175 writes of 4096 bytes per 5 seconds
would be 143360 B/s. That is equivalent to 28.57 ms per block versus 23.22 ms of
audio, but the difference does **not** prove a hidden 5.35 ms sleep. Cumulative
counters without exact interval timestamps cannot establish that rate reliably.

The internal Bluedroid SBC decoder writes signed `OI_INT16` samples directly to
the application callback. On ESP32 these are little-endian, interleaved L/R when
the negotiated mode is stereo, joint stereo or dual channel. Its channel count
follows negotiation; mono must not be interpreted as stereo. Mono and malformed
packet lengths are now rejected and counted as `invalid_pcm` (also included in
`dropped`). The nonblocking callback enqueues only whole four-byte frames, with
zero wait, simple counters and no logging or I2S calls. No bytes, bits or channels
are swapped. The Philips, 16-bit stereo I2S configuration matches this PCM layout.

References: [Bluedroid decoder source](https://github.com/espressif/esp-idf/blob/v6.1/components/bt/host/bluedroid/btc/profile/std/a2dp/btc_a2dp_sink.c),
[I2S buffer layout and API](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/i2s.html).

The local toolchain is PlatformIO Espressif32 7.1.3 / ESP-IDF 6.1.0. The existing
generated configuration already contained changes from 6.0.1 to 6.1.0 before
this investigation; those changes were preserved. The relevant blocking write
loop in [6.0.1](https://github.com/espressif/esp-idf/blob/v6.0.1/components/esp_driver_i2s/i2s_common.c)
and [6.1](https://github.com/espressif/esp-idf/blob/v6.1/components/esp_driver_i2s/i2s_common.c)
has the same DMA wait/copy semantics and no post-write sleep. 6.1 adds DMA-path
validation there. That comparison does not exclude a lower-level clock or driver
regression; neither the platform nor IDF version was changed as a workaround.

## Bluetooth verification

1. Keep all five diagnostic flags in `src/app_config.h` at `0`, build and manually
   upload using the procedure above. If `pio` is not on PATH in PowerShell, use
   `& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run` (and the same
   executable with `run -t upload` or `device monitor -b 115200`).
2. Connect the phone to `Faital Speaker Proto`, play music for at least 60 seconds
   and retain the startup/SBC configuration log and consecutive statistics lines.
3. After initial prefetch, expect `sample_rate=44100`, `audio_state=started`,
   `mode=playing`, `rx_rate` near 176400 B/s and `i2s_rate` near `rx_rate`.
   Small variations between five-second intervals are normal. At a negotiated
   48000 Hz the expected rate is 192000 B/s instead.
4. Check `dropped=0`, `invalid_pcm=0`, `latency_trim=0/0events`, `err=0`, `short=0`
   during steady playback. The buffer should fluctuate without continuously
   growing toward its limit, and underflows/LED error patterns should not recur.
5. Pause/resume and disconnect/reconnect. Check the connected/streaming LED states
   and the saved-phone reconnection attempt after reboot.

Statistics are emitted every five seconds, including while stopped. `cb` and
`(+...)` are cumulative and interval callback counts. `rx_interval` and
`i2s_interval` are byte deltas; both rates use the actual elapsed monotonic time.
`i2s` counts bytes accepted by the driver, not a measurement on the I2S pins.
`buffer_max` is the highest occupancy observed after callback enqueue since boot;
the concurrent consumer can drain data between enqueue and observation.
`write_us(avg/max)` is the interval average / lifetime maximum time inside the
I2S write API, including its waits and scheduling. `dropped`, `invalid_pcm`, trim,
errors and underflows are cumulative; `invalid_pcm` is a subset of `dropped`.
The stopped audio state covers suspension and disconnection; Bluetooth event
logs distinguish them. `sample_rate` reports the applied configuration, not a
physical clock measurement. GPIO, AVRCP, NVS/MAC storage and LED patterns remain
as configured.

## Isolated I2S diagnostic tone

Set only `AUDIO_DIAGNOSTIC_TONE_ENABLED` to `1` in `src/app_config.h`; leave drain
and bypass at `0`. Build and manually upload. Keep amplifier volume low initially.
This build skips Bluetooth startup and runs the **only** audio writer with a
1000 Hz sine at 44100 Hz, signed 16-bit stereo, identical L/R and a peak of 3276
(10% of full scale). The same I2S configuration, DMA and write/retry path are used.
The sine table is calculated once before playback. Expect a steady clean tone,
`mode=diagnostic-tone`, `audio_state=disabled-tone`, `rx_rate=0`, and `i2s_rate`
near 176400 B/s. Compile-time checks reject incompatible diagnostic flags.

After testing, return the tone flag to `0`, rebuild and manually upload to restore
Bluetooth playback. A normal build never starts the tone. Firmware compilation
alone cannot verify the audible result; no board upload was performed during
this investigation.

If the problem persists, retain at least 12 consecutive statistics lines for
both modes. Compare write duration and rates, then measure GPIO25 WS (about
44.1 kHz) and GPIO26 BCK (about 1.4112 MHz for the configured two 16-bit slots).
Capture GPIO22 DATA with a Philips I2S decoder to check frame alignment and the
1000 Hz waveform. A slow tone-build write rate points to the I2S/clock/scheduling
path independently of Bluetooth. A clean tone and stable rates with Bluetooth
noise require inspecting the negotiated SBC mode and decoded PCM. Correct PCM
on the pins with noisy analog output directs the next check to the existing
DAC/amplifier path. The subsequent clean-tone result localizes the current deficit
upstream of the PCM input; follow the Bluetooth/HCI investigation linked above
for the next tests.

