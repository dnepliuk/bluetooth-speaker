# Faital A2DP Control

Незалежна A/B-матриця Bluetooth Classic A2DP Sink для DOIT ESP32 DEVKIT V1
(ESP32/WROOM-32), USB-живлення, без підключеної периферії. Working drain-only
baseline збережений; додано окремі перевірки main sdkconfig, StreamBuffer,
I²S цифрової тиші, мінімального pipeline, I²S-clocked pipeline, SPSC pipeline
та окремого **`control-spsc-dma-aligned`** із writer block **3840 bytes**.
Поточний тест — **матриця CPU frequency × Bluetooth controller modem sleep**
на спільному `control-official-i2s-reference`: BYTEBUF → I²S MSB.

Відкрийте **цю папку окремим вікном VS Code**. У PlatformIO → Project Tasks
оберіть environment, потім **General → Build / Upload** та
**Platform → Monitor** (115200). Натисніть EN/RESET і перевірте banner.
Редагувати defines або запускати Upload через PowerShell не потрібно.

| Наявний environment | Banner | Що перевіряє |
|---|---|---|
| `control-baseline` | `CONTROL variant=baseline` | Повтор working drain-only |
| `control-main-config` | `CONTROL variant=main-config` | CPU 240 MHz + modem sleep off |
| `control-streambuffer` | `CONTROL variant=streambuffer` | StreamBuffer + consumer без I²S |
| `control-i2s-silence` | `CONTROL variant=i2s-silence` | Drain-only PCM + незалежний I²S writer тиші |
| `control-pipeline` | `CONTROL variant=pipeline` | PCM → StreamBuffer → writer → I²S |
| `control-clocked-pipeline` | `CONTROL variant=clocked-pipeline` | Постійний I²S writer, receive timeout=0, PCM + тиша, prefill 8192 |
| `control-spsc-pipeline` | `CONTROL variant=spsc-pipeline` | Той самий clocked writer; StreamBuffer замінено статичним SPSC 32768 bytes |
| `control-spsc-dma-aligned` | `CONTROL variant=spsc-dma-aligned` | Той самий SPSC writer, output 4096 → 3840 bytes; prefill 8192, DMA 3×960 без змін |
| `control-official-i2s-reference` | `CONTROL env=control-official-i2s-reference variant=official-i2s-reference` | Official-style BYTEBUF 32768 / prefetch 20480, DMA 6×240, receive до 1440 bytes, unpinned writer; CPU 160 MHz / sleep ON |
| `control-official-160-nosleep` | `CONTROL env=control-official-160-nosleep variant=official-i2s-reference` | Той самий reference, CPU 160 MHz / modem sleep OFF |
| `control-official-240-sleep` | `CONTROL env=control-official-240-sleep variant=official-i2s-reference` | Той самий reference, CPU 240 MHz / modem sleep ON |
| `control-official-240-nosleep` | `CONTROL env=control-official-240-nosleep variant=official-i2s-reference` | Той самий reference, CPU 240 MHz / modem sleep OFF |

Для кожного: той самий локальний трек, Wi-Fi/hotspot off, відстань 20–30 см,
90 s відтворення, ≥12 послідовних stats groups. Закрийте Monitor перед
наступним Upload. Межі PASS при 44100 Hz — 174636–178164 B/s, без регулярних
sequence warnings, gaps >100 ms, drops або I²S errors/short writes.

Повний [протокол A/B і таблиця результатів](docs/ab-investigation.md) пояснює
правильні пари порівняння, семантику нових counters і всі відхилення від main.
[Порівняння sdkconfig](docs/sdkconfig-comparison.md) містить аудит параметрів.
Надані користувачем апаратні результати; історичні значення перших п'яти тестів збережені:

| Environment | Результат | Середній PCM B/s | Додаткові дані |
|---|---|---|---|
| baseline | PASS | 176299.9 (99.943%) | Без sequence errors і gaps >100 ms |
| main-config | PASS | 176264.5 | CPU 240 MHz, sleep off; без sequence errors і gaps >100 ms |
| streambuffer | PASS | 176274.5 | dropped=0; callback/send max 449/294 us; без sequence errors і gaps >100 ms |
| i2s-silence | PASS | 176218.8 | I²S 176394 B/s, errors/shorts=0; без sequence errors і gaps >100 ms |
| pipeline | FAIL | 145947.4 (82.737%), без фінального catch-up | ~215 sequence errors, 1 HCI reassembly error; gaps 100–266 ms; callback/send max 2558/2458 us; I²S errors/shorts=0 |
| clocked-pipeline | FAIL | 141125.9 (80.0%) | 175 sequence errors; gaps ~280–304 ms; I²S ~176400, errors/shorts=0; silence ~229–250 KiB/5 s |
| spsc-pipeline | FAIL | ~151296 | 172 sequence errors; gaps 270–291 ms; I²S ~176400, errors/shorts=0; drops ~86–131 KiB/5 s, silence ~229–246 KiB/5 s |
| spsc-dma-aligned | **Historical FAIL** | ~141200 | За новим повідомленням користувача: sequence errors ~кожні 350 ms, I²S err/short=0; коротке відновлення наприкінці не є PASS |
| official-i2s-reference | **Historical FAIL** | ~152000 | CPU 160/sleep ON: sequence errors ~кожні 290 ms, underflows/application drops, I²S err/short=0; коротке відновлення не є PASS |

У failing pipeline fill коливався 0–32768 і були drops. Фінальні 176467 B/s
із fill=28672 та dropped_interval=8192 — catch-up, а не відновлення потоку.

Clocked FAIL спростовує пояснення дефекту лише 100-мс blocking receive.
Попередня A/B-пара `control-clocked-pipeline → control-spsc-pipeline` замінила
тип буфера, проте дефект залишився без StreamBuffer. Причина не обмежується
StreamBuffer. У clocked також були drops ~40–65 KiB/5 s,
fill 0–32768, send/callback maxima ~2146/2253 us.

Reference clocked/SPSC writers передають блоки 4096 bytes, aligned — 3840:
PCM на початку, нулі у відсутньому
хвості. Поки накопичуються 8192 bytes, він передає повні блоки тиші;
underflow не запускає prefill повторно. Callback, baseline CPU 160 MHz / modem
sleep, capacity 32768/frame 4, priority 22/core 1/stack 8192 та DMA 3×960
збережені. Старий `control-pipeline` із receive timeout 100 ms залишається
failing reference, як і `control-clocked-pipeline`. Очищення при зміні стану
виконує тільки reader; producer fence і неминучий хвіст DMA описані в протоколі.

## Поточна CPU × modem sleep матриця

| Порядок фізичного запуску | Environment | CPU | Controller modem sleep | Новий hardware run |
|---|---|---:|---|---|
| 1 | `control-official-160-nosleep` | 160 MHz | OFF | **NOT RUN** |
| 2 | `control-official-240-sleep` | 240 MHz | ON | **NOT RUN** |
| 3 | `control-official-240-nosleep` | 240 MHz | OFF | **NOT RUN** |
| 4 — повторний контроль умов | `control-official-i2s-reference` | 160 MHz | ON | **NOT RUN** |

Усі чотири environments мають selector **9** і ті самі два C sources.
Аудіотракт, callbacks, writer, buffering, scheduling, GPIO, MSB, DMA і stats
не змінено. Єдині фактори — CPU/sleep. Зміни коду обмежені startup-діагностикою:
унікальне ім'я environment, скомпільовані CPU/sleep settings і одноразове читання
CPU clock через публічний `esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU, ...)`.
Рядок PlatformIO `HARDWARE: ESP32 240MHz` описує плату; перевірені CPU settings
беруться зі `sdkconfig/sdkconfig.h`, а фактичний clock — зі startup UART.

Параметри локального ESP-IDF 6.1.0:

- `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y` або `_240=y`; числовий
  `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` генерує Kconfig.
- `CONFIG_BTDM_CTRL_MODEM_SLEEP=y/n` обирає controller low-power mode.
  ON зберігає ORIG і MAIN_XTAL; OFF вимикає modem sleep. Це не sniff policy
  і не ESP32 light sleep; `CONFIG_PM_ENABLE` лишається вимкненим.
- CPU choice siblings і legacy `CONFIG_ESP32_DEFAULT_CPU_FREQ*` змінюються
  автоматично; для sleep OFF зникають enabled ORIG/LPCLK selection і legacy
  `CONFIG_BTDM_CONTROLLER_MODEM_SLEEP`. Інші sdkconfig values мають збігатися.

Кожен environment має власні `sdkconfig.<environment>`, build directory і
defaults overlay. Встановлений PlatformIO builder підтримує
`board_build.esp-idf.sdkconfig_path`; CMake отримує окремий `SDKCONFIG_DEFAULTS`.
Read-only pre/post guard перевіряє також **наявний generated sdkconfig і header**:
defaults не можуть приховати застарілі значення. При невідповідності Build
зупиняється, конфіг не переписується guard-ом. Clean/Menuconfig доступні для
виправлення лише відповідного environment. Framework/global packages не змінюються.

### Запуск через VS Code UI

1. Окреме вікно: `C:\bluetooth-speaker\control\a2dp_sink_control`.
2. **PlatformIO → Project Tasks → потрібний environment → General → Build → Upload**.
3. **Platform → Monitor**, **115200**, потім **EN/RESET**.
4. Перевірте `CONTROL env=... variant=official-i2s-reference`,
   `cpu_config_mhz=... bt_modem_sleep_config=enabled/disabled` і
   `cpu_runtime_hz=... clock_read_result=0`. Sleep setting не є runtime-вимірюванням.
5. Перевірте незмінні ring=32768, prefetch=20480, DMA 6×240, write_upto=1440,
   priority=22, unpinned, MSB/no audible validation. Телефон: **Faital A2DP Control**.
6. Та сама ESP32/USB/телефон/розташування, **без DAC/підсилювача**,
   Wi-Fi/hotspot вимкнені. Той самий завантажений трек із **тієї самої позиції**.
7. **90 секунд** без pause/seek/зміни гучності; **потім pause**.
8. Кожен UART — окремий файл `<environment>.log`, від RESET, із banner,
   SBC/MTU/audio states, ≥12 повними stats intervals та всіма warnings.
9. **Закрийте Monitor перед наступним Upload**. Дотримуйтеся порядку таблиці вище.
10. Якщо є PASS, **повторіть цей environment**, перш ніж вважати результат відтворюваним.

| Environment | PCM / expected, B/s | Sequence warnings / period | Drops / underflows | I²S err / short | Новий результат / повтор |
|---|---|---|---|---|---|
| control-official-160-nosleep | — | — | — | — | NOT RUN |
| control-official-240-sleep | — | — | — | — | NOT RUN |
| control-official-240-nosleep | — | — | — | — | NOT RUN |
| control-official-i2s-reference — повтор | — | — | — | — | NOT RUN; попередній запуск historical FAIL |

Expected = negotiated sample rate × channels × 2 bytes: **176400 B/s** для
44.1 kHz stereo s16, **192000 B/s** для 48 kHz stereo. Оцінюйте кілька повних
вікон після prefetch. PASS: PCM близький до expected, без регулярних sequence
errors, application drops/underflows, I²S err/short=0. Counters накопичувальні:
порівнюйте їхні прирости під час сталого відтворення. Короткий catch-up наприкінці
або один добрий рядок — не PASS. Різниця покаже вплив CPU/sleep/їх взаємодії,
але сама не доведе механізм дефекту. Поверніть усі чотири логи й повтор будь-якого PASS.

Докладний аудит Kconfig, controller init і залежних параметрів — у
[розділі 10 протоколу](docs/ab-investigation.md#10-cpu-frequency--bluetooth-modem-sleep).

### Збірки матриці — 2026-09-17–18

| Environment | Build | RAM, bytes | Flash code/data, bytes |
|---|---|---:|---:|
| control-official-i2s-reference | PASS, включно з повтором після решти | 54528 | 759545 |
| control-official-160-nosleep | PASS | 54528 | 758273 |
| control-official-240-sleep | PASS | 54528 | 759561 |
| control-official-240-nosleep | PASS | 54528 | 758709 |

**Config-isolation PASS:** окремі sdkconfig/build paths, config/header parity,
жодних змін сторонніх конфігів під час збірки, reference після перемикання
має ті самі settings. Відмінних keys від reference: **4 / 6 / 10** відповідно
для 160/OFF, 240/ON і 240/OFF; лише CPU/sleep, залежності та legacy aliases.
У generated reference Kconfig прибрав чотири коментарі `# default:` через явні
defaults; усі значення збережено. Guard пройшов **28/28** перевірок.

Official і SPSC host-тести пройшли. Початкові блокування Windows Application
Control зафіксовані у звіті; ті самі команди згодом завершилися успішно без змін
політики безпеки, toolchain чи коду для обходу блокування.
**Verification PASS:** `verify_ab` разом із clocked/SPSC/aligned/official/matrix
перевірками — exit 0; **24/24** попередні preprocessed одиниці й **26/26** файлів
основної прошивки незмінні. Вісім попередніх environments також зібрано успішно.
Історичні ELF hash mismatches лишаються окремими advisory FAIL; expected hashes
не змінювалися. `git diff --check` і whitespace-перевірка змінених файлів пройшли.
**Hardware — NOT RUN**; агент не запускав Upload/Monitor і не робив commit/push.
Повний машинний звіт: [docs/verification.json](docs/verification.json).

## Історичний official I²S reference: архітектура

Це окремий architecture reference: він змінює кілька параметрів одночасно.
Мета — перевірити, чи відтворюється дефект із локальним офіційним output service.
Попередні вісім variants і основна прошивка збережені. CPU **160 MHz**,
modem sleep **увімкнений**, як у pipeline/SPSC (цей вибір підтверджено користувачем).
Окремий `control-main-config` має інші налаштування: 240 MHz / sleep off.

| Параметр | DMA-aligned | Official reference | Локальний ESP-IDF 6.1 source |
|---|---|---|---|
| Буфер | Custom SPSC, 32768 bytes | BYTEBUF, 32768 bytes | `RINGBUF_TYPE_BYTEBUF`, 32×1024 |
| Prefetch | 8192, тільки початковий | 20480, початковий і після underflow | 20×1024, PREFETCHING |
| Overflow | Приймається доступний frame-aligned prefix, решта drop | Цілий пакет або drop; DROPPING до fill ≤20480 | Та сама state machine; пакет виходу з DROPPING теж відкидається |
| Writer read | До 3840, timeout 0 | До 1440, timeout 20 ms | `xRingbufferReceiveUpTo`, 240×6 **bytes** |
| Software zero-padding | До повного блока 3840 | Немає | Немає; `auto_clear=true` у драйвері |
| DMA / interrupt priority | 3×960 / 0 | 6×240 / 0 | `I2S_CHANNEL_DEFAULT_CONFIG`, без override |
| I²S slot format | Philips/I²S | MSB, s16, stereo за замовчуванням | `I2S_STD_MSB_SLOT_DEFAULT_CONFIG` |
| Writer priority/core/stack | 22 / core 1 / 8192 | 22 / unpinned / 4096 | `configMAX_PRIORITIES-3`, `xTaskCreate`, 4×1024 |
| Write timeout / short write | 1000 ms / retry залишку | `portMAX_DELAY` / лише counters | `portMAX_DELAY` / без retry та перевірки результату |
| Частота | Стартова 44100, далі зі SBC через writer | Із SBC event, включно 44100/48000 | SBC helper: 16000/32000/44100/48000, mono/stereo |
| GPIO BCK/WS/DATA | 26/25/22 | 26/25/22, MCLK unused | `CONFIG_EXAMPLE_I2S_*`, MCLK unused |
| Lifecycle | Постійний custom transport | Open CONNECTING, start CONNECTED, stop/close DISCONNECTED | Такі самі connection hooks; деталі teardown описано в протоколі |
| PCM-діагностика | Timing, critical-section stats | 32-bit relaxed counters, без timer/log/очікування | Helper логує кожні 100 пакетів; output service також має hot-path logs |

Повне походження, intentional deviations і межі перевірок наведено в
[розділі 9 протоколу](docs/ab-investigation.md#9-official-i2s-reference).
Немає application zero-padding; увімкнений `auto_clear` може обнулювати DMA
під час простою. `i2s_written/rate` рахують байти, прийняті `i2s_channel_write`,
а не фізично передані автоматично очищені DMA-байти.

### Ручний official reference тест у VS Code

1. Відкрийте окремим вікном `C:\bluetooth-speaker\control\a2dp_sink_control`.
2. **PlatformIO → Project Tasks**.
3. Оберіть **control-official-i2s-reference**.
4. **General → Build**.
5. Закрийте старий Monitor, потім **General → Upload**.
6. **Platform → Monitor**, **115200**; за потреби оберіть COM-порт.
7. Натисніть **EN/RESET**.
8. Перевірте banner нижче; підключіть телефон до **Faital A2DP Control**.
9. Живлення — лише **ESP32 через USB**.
10. **DAC і підсилювач не підключати**; звук/MSB не оцінюється.
11. **Wi-Fi/hotspot телефона вимкнути**.
12. Відстань **20–30 см**.
13. Той самий завантажений локальний трек.
14. Відтворюйте **90 секунд** без pause/seek.
15. Збережіть UART від RESET: banner, SBC/MTU/audio states, не менше
    **12 послідовних `OFFICIAL stats`**, усі `BT_APPL` / `BT_HCI` warnings.

Очікувані поля banner (значення виводяться зі скомпільованих параметрів):

```text
CONTROL variant=official-i2s-reference
CONTROL source=ESP-IDF-6.1 a2dp_sink_stream
CONTROL ring=32768 prefetch=20480
CONTROL i2s=dma_desc_num=6 dma_frame_num=240 write_upto=1440 task_priority=22 task_core=unpinned
CONTROL output_format=MSB official reference; no audible validation
```

PASS при 44.1 kHz stereo: сталий PCM близько **176400 B/s**, після початкового
prefetch немає регулярних underflows/sequence errors, немає `Pkt dropped`,
`dropped_packets=0`, I²S `err=0`, `short=0`. При 48 kHz stereo expected=192000 B/s.
Стартові/idle/перехідні вікна не є сталим потоком. Counters накопичуються від boot;
`pcm_interval`, rates та `elapsed_ms` описують поточний інтервал. Mode:
0=PROCESSING, 1=PREFETCHING, 2=DROPPING. Після навмисної pause очікується re-prefetch.

FAIL: повторні PCM 141–151 kB/s при 44.1 kHz, sequence errors, систематичні
underflows або drops. PASS підтримає пошук у відмінностях custom transport/writer
чи scheduling; аналогічний FAIL змістить увагу до спільного I²S/BT/IDF/плати.
Цей тест сам по собі не визначає конкретну першопричину.

Поверніть повний UART-лог, модель телефона/плеєр, negotiated sample rate,
тривалість і умови запуску. Окремо після основних 90 s перевірте disconnect/reconnect
та нове відтворення й збережіть ці transition-логи, не змішуючи їх зі сталими вікнами.

### Перевірки official reference — 2026-09-16

Усі **9 builds — SUCCESS**. Новий ELF: RAM **54528 bytes**, flash **759229 bytes**.
**24/24** preprocessed одиниці попередніх восьми variants і їхні sdkconfigs
незмінні; **26/26 root files** незмінні. `main.c` і custom transports не редагувалися.

`verify_ab`, `verify_clocked`, `verify_spsc`, `verify_aligned`, `verify_official`,
обидва runtime verifiers і `git diff --check` — **PASS / exit 0**.
Нові host-тести перевіряють production service із API adapters; старі SPSC-тести
повторено для 4096/3840. Фізичні Bluetooth/DMA tests залишаються **NOT TESTED**.

Historical ELF hashes залишаються окремим **advisory FAIL**; поточне завдання
не вимагає їхньої byte identity. Expected references не переписані. Опція
`--strict-elf` у `verify_spsc.py` / `verify_aligned.py` повертає попередній суворий
критерій. Повний звіт, включно історичним aligned-раундом:
[docs/verification.json](docs/verification.json).

## Попередня A/B-пара: SPSC → DMA-aligned

Єдина функціональна відмінність — `CONTROL_CHUNK_BYTES: 4096 → 3840`.
Prefill **8192** (не 7680), capacity **32768**, frame **4**, DMA **3×960**,
GPIO **26/25/22**, 44100 Hz, writer **priority 22/core 1/stack 8192**,
CPU/modem sleep, повний sdkconfig, callback/producer і агреговані логи збережені.
Новий boot log лише явно показує вирівнювання.

Гіпотеза: у старій парі розміри повторно збігаються через
`15 × 4096 = 16 × 3840 = 61440 bytes`, або **0.3483 s** при 176400 B/s.
Користувач повідомив про sequence errors переважно кожні 0.34–0.35 s та
приріст `recv` приблизно на `0x0F`. Це кореляція; причинність ще не перевірена.
У aligned очікуються **229–230 writes / 5 s** та **~21.77 ms/write**.
Для вікна іншої тривалості порівнюйте з `176400 × elapsed_seconds / 3840`.

### Ручний DMA-aligned тест у VS Code

1. Відкрийте окремим вікном `C:\bluetooth-speaker\control\a2dp_sink_control`.
2. **PlatformIO → Project Tasks → control-spsc-dma-aligned**.
3. **General → Build**.
4. Закрийте попередній Monitor, потім **General → Upload**.
5. **Platform → Monitor**, **115200**; за потреби оберіть COM-порт ESP32.
6. Натисніть **EN/RESET**.
7. Перевірте `CONTROL variant=spsc-dma-aligned`, `output_bytes=3840`,
   `dma_descriptor_bytes=3840`, `dma_frames=960`, `frame_bytes=4`,
   `writer_dma_aligned=yes` та незмінний `prefill_bytes=8192`.
8. Підключіть телефон до **Faital A2DP Control**.
9. Вимкніть **Wi-Fi/hotspot**.
10. Той самий завантажений трек, відстань **20–30 см**.
11. Лише **ESP32 через USB**, без DAC і підсилювача.
12. Відтворюйте **60–90 секунд**, бажано 90, без pause/seek.
13. Збережіть UART від RESET: banner, SBC/MTU/audio states, всі Bluetooth warnings
    та ≥12 послідовних груп **`stats` / `SPSC` / `i2s` / `clocked`**.

PASS: стабільна PCM rate близько 176400 B/s (±1%, не нижче 174636), без регулярних
sequence errors і gaps >100 ms; drops після стартового періоду = 0; inserted
silence після prefill близька до нуля; I²S близько 176400 B/s, errors/shorts = 0;
~229–230 writes за повні 5 s. Додатково порівняйте часові інтервали між sequence
errors. Зникнення помилок підтримає гіпотезу; зміна їхнього періоду вкаже на
залежність від writer/DMA phase; незмінні ~0.35 s послаблять гіпотезу, після
чого окремим A/B можна перевіряти priority. Поточний priority не змінено.

Кожен штатний I²S write має 3840 bytes. Успадкована обробка аварійного partial
write дописує лише залишок; zero-progress error має той самий backoff 1 tick.
Нових затримок немає. Errors/shorts роблять апаратний тест FAIL; зміна recovery
була б другою функціональною змінною. При underflow блок доповнюється нулями
до 3840, prefill повторно не запускається; output buffer також має 3840 bytes.

## Ручний SPSC-тест у VS Code

Ці кроки залишені для повтору failing reference із блоком 4096 bytes.

1. Відкрийте окремим вікном папку `C:\bluetooth-speaker\control\a2dp_sink_control`.
2. **PlatformIO → Project Tasks → control-spsc-pipeline → General → Build**.
3. У цьому самому environment: **General → Upload**. Перед цим закрийте старий Monitor.
4. **Platform → Monitor**, швидкість **115200**; за потреби виберіть COM-порт ESP32.
5. Натисніть **EN/RESET**.
6. Перевірте banner **`CONTROL variant=spsc-pipeline`**, параметри writer та I²S.
7. Підключіть телефон до **Faital A2DP Control**.
8. Wi-Fi/hotspot вимкнені; той самий завантажений трек; відстань **20–30 см**.
   Збережіть ту саму апаратну конфігурацію попереднього A/B (ESP32 + USB).
9. Програвайте **60–90 секунд**, бажано 90, без pause/seek.
10. Збережіть весь UART від RESET: banner, SBC/MTU/audio states, щонайменше
    **12 послідовних груп `stats` / `SPSC` / `i2s` / `clocked`**, усі Bluetooth warnings.

PASS: після початкового prefill PCM та I²S близько 176400 B/s (PCM ±1%,
174636–178164), немає регулярних sequence errors, gaps >100 ms або постійних
SPSC drops; inserted silence близька до нуля; I²S errors/short writes = 0.
Рахуйте зважену PCM rate за повними вікнами. Окремо після основного тесту
перевірте pause/resume та disconnect/reconnect і збережіть transition-логи.

`SPSC` замінює п'ятисекундний buffered `transport`-звіт, зберігаючи callback
timing, invalid/discarded counters; `stats`, `i2s`, `clocked` збережені.
`overflow_events` і `wrap_writes/reads` — прирости за інтервал; wrap означає
два `memcpy`. Watermarks і maxima — накопичені від boot, як у reference;
fill — останній sample одного з власників, а не синхронний знімок обох ядер.

SPSC використовує статичну внутрішню DRAM і 32-bit unsigned counters.
Producer публікує write через release після копіювання; consumer читає через
acquire, а read публікує release після копіювання. Використані
[GCC `__atomic` builtins](https://gcc.gnu.org/onlinedocs/gcc/_005f_005fatomic-Builtins.html)
перевірені на встановленому Xtensa GCC 15.2: inline load/store + `memw`, без
atomic helper calls. Буфер не використовує locks, notifications або allocation.
Існуючі короткі critical sections статистики callback/transport збережені
для чесного A/B; весь callback не оголошується wait-free.

Результати збірок і перевірок — у [машинному звіті](docs/verification.json).
Verifier зберігає попередні перевірки й додатково порівнює 20 preprocessed одиниць
семи variants із snapshot до aligned. Код і конфігурації перевіряються окремо
від allocated ELF sections, крім build metadata.
Основні 26 файлів захищені SHA-256;
baseline callback незмінний. Upload і Monitor агент не запускає.
В історичному звіті додавання SPSC (2026-09-15) всі сім Build пройшли, але
**строга побайтова рівність ELF — FAIL для i2s-silence та clocked-pipeline**.
Незмінна чиста перезбірка main-config продемонструвала зміну порядку бібліотек
у linker map і відповідних ELF bytes. Очікувані hashes не переписані;
`verify_ab.py` / `verify_spsc.py` повертають exit 1 через цю різницю. Докладні
результати та межі гарантій наведені в A/B-протоколі. Актуальний звіт восьми
environments — `docs/verification.json`: source/preprocessed equality,
semantic checks і strict ELF results мають окремі поля; hash failure не
приховується. Host-тести запускають production ring окремо з reads 4096 і 3840,
зокрема 128 різних позицій кільця для 3840, rollover, overflow, flush і тривалий
producer/consumer. До фізичного PASS зміни в основний застосунок не переносяться.

Поточна перевірка **2026-09-16**: всі **8 builds SUCCESS**, source/preprocessing
**20/20 PASS**, semantic checks і обидва host-тести **PASS**, 26 root-файлів
незмінні, `git diff --check` **PASS**. Strict ELF проти snapshot перед aligned:
**FAIL для pipeline, clocked-pipeline, spsc-pipeline**; linker maps показують
змінений LOAD order бібліотек. Старий SPSC-verifier додатково звіряє старіший
snapshot і має FAIL для i2s-silence/pipeline. Ці hash failures збережені окремо
від semantic PASS; `verify_ab.py`, `verify_spsc.py`, `verify_aligned.py` мають exit 1.

Далі описаний збережений baseline та його походження.

## Джерело та відмінності

Головне джерело — встановлений ESP-IDF **6.1.0**, пакет
`platformio/framework-espidf@4.60100.0`:

```text
C:\Users\1dima\.platformio\packages\framework-espidf\examples\bluetooth\bluedroid\classic_bt\a2dp_sink_stream
```

Використано `main/main.c`, `sdkconfig.defaults`, `CMakeLists.txt`, а також
сусідні `common/bredr_app_common_utils/bredr_app_common_utils.c` і
`common/a2dp_utils/a2dp_sink_int_codec_utils/a2dp_sink_int_codec_utils.c`.
Перевірено також `a2dp_sink_common_utils.c` та API-заголовки встановленого IDF.
Онлайн-джерело: [Espressif, a2dp_sink_stream v6.1](https://github.com/espressif/esp-idf/tree/v6.1/examples/bluetooth/bluedroid/classic_bt/a2dp_sink_stream).
Copyright/SPDX-заголовок збережено у `src/main.c`.

Усі відмінності від прикладу:

- Окремий PlatformIO-проєкт із `src` замість `main`, власною назвою збірки,
  закріпленими платформою 7.1.3 та IDF 6.1.0, платою і UART 115200.
  CMake зберігає `MINIMAL_BUILD`; локальний `.gitignore` виключає артефакти.
- Назва Bluetooth — `Faital A2DP Control`, задана константою. Немає власного
  Kconfig чи `idf_component.yml`: потрібні тільки компоненти IDF.
- Не підключені example helpers з їхньою задачею `bt_app`, чергою та копіями
  подій. Їхню потрібну послідовність NVS → controller → Bluedroid → A2DP
  перенесено в `app_main`; рідкісні події обробляються прямо в A2DP callback.
  Повернені помилки API перевіряються через `ESP_ERROR_CHECK`.
- Видалено audio sink service, I²S/DAC, audio writer, буфери та вибір
  зовнішнього декодера. PCM callback лише рахує дані й часові проміжки.
  Він використовує той самий `esp_a2d_sink_register_data_callback`, що й
  default-гілка офіційного прикладу; у 6.1 цей API розташований у legacy header,
  включеному через `esp_a2dp_api.h`, і залишається доступним.
- Замість логу кожних 100 пакетів додано одну задачу статистики, priority 1,
  stack 4096 bytes, без прив'язки до ядра. Вона прокидається кожні 100 мс для
  відкладеного першого PCM-логу і перевірки п'ятисекундного інтервалу.
- Немає запиту назви через device callback, додавання 5 мс до delay report
  або логування другорядних GAP/profile-подій. Delay reporting залишається
  зі штатним значенням стека. Збережено SSP, I/O capability `ESP_BT_IO_CAP_IO`,
  автоматичне підтвердження SSP і legacy PIN `1234`, як у common helper.
  Збережено перемикання discoverability після connect/disconnect.
- Додано версію/назву збірки, connection/MAC/MTU, raw SBC, частоту, channel mode,
  audio state та перевірку зміни/некоректності формату. Частота читається з
  `mcc.cie.sbc_info.samp_freq`; множинні/невідомі біти дають 0, без припущення 44100.
- У API 6.1 є тільки `SUSPEND` і `STARTED`. Локальний `stopped` означає початковий
  стан або від'єднання; натискання Stop на телефоні може дати `suspended`.
- Із defaults вилучено тільки `CONFIG_DAC_DMA_AUTO_16BIT_ALIGN=n`: DAC не
  використовується. Явно закріплено внутрішній SBC-декодер та UART, як нижче.
  Додано flash 4 МБ відповідно до опису плати DOIT: default IDF 2 МБ викликав
  попередження PlatformIO про невідповідність. Таблиця розділів — штатна IDF.

Не додано AVRCP API, reconnect, прикладного MAC storage, RSSI, LED, DSP,
prefetch або latency trimming. **У самому IDF `BT_A2DP_ENABLE` примусово
вибирає `BT_AVRCP_ENABLED=y`**; без патчу IDF вимкнути цей залежний символ не
можна. Control не ініціалізує AVRCP CT/TG і не реєструє їх callbacks.
NVS та штатне збереження pairing keys стеком залишаються стандартними.
Розміри Bluetooth-буферів, пріоритети його задач і controller settings не змінені.

## sdkconfig.defaults

```ini
CONFIG_BT_ENABLED=y
CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY=y
CONFIG_BT_BLUEDROID_ENABLED=y
CONFIG_BT_CLASSIC_ENABLED=y
CONFIG_BT_A2DP_ENABLE=y
CONFIG_BT_BLE_ENABLED=n
CONFIG_BT_A2DP_USE_EXTERNAL_CODEC=n
CONFIG_ESP_CONSOLE_UART_DEFAULT=y
CONFIG_ESP_CONSOLE_UART_BAUDRATE=115200
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
```

ESP32 обирається платою PlatformIO. Інші параметри — defaults IDF, зокрема
INFO logging і WARNING для BT_APPL/BT_HCI. Штатні `Sequence numbers error`
та `reassemble_and_dispatch found unfinished packet` не перехоплюються,
не приглушуються і не рахуються застосунком. Повний згенерований конфіг
зберігається окремо для кожного environment у `sdkconfig.control-*`.
Історичний `sdkconfig.a2dp_sink_control` також збережено. BOYA support не змінено.

## Як читати статистику

`callbacks` та 64-bit `pcm_total` накопичуються від завантаження. `(+...)` і
`pcm_interval` — приріст за `elapsed_ms`. Швидкість обчислюється як
`pcm_interval * 1000000 / elapsed_us`, із `esp_timer_get_time()` у момент
узгодженого snapshot; мілісекунди й B/s у логах округлені вниз.
`expected_rate = sample_rate * 2 * 2`: для stereo PCM s16le, interleaved L/R,
це 176400 B/s при 44100 Hz і 192000 B/s при 48000 Hz. Mono/невідомий channel
mode потребує окремої перевірки; формулу stereo не слід трактувати як доказ втрат.

Новий STARTED, зміна формату, suspend або disconnect скидають вимірювальне
вікно й історію пауз; накопичені totals залишаються. Статистика друкується
тільки у STARTED, кожні щонайменше 5 реальних секунд. Навіть без жодного PCM
вона показує нульову швидкість і зростання `no_pcm_now_ms`. Перший інтервал
включає очікування даних після STARTED; порівнюйте кілька наступних інтервалів.

`last_size` — розмір останнього callback від завантаження.
`gaps_gt_*` — кількість завершених проміжків між callbacks за інтервал,
строго понад 30/50/100/200 мс; пороги вкладені, одна пауза 220 мс збільшує всі
чотири лічильники. Пауза через межу статистичних вікон рахується у вікні,
де прийшов наступний callback. `no_pcm_now_ms` — час після останнього PCM
поточного активного сегмента, або після його початку, якщо PCM ще не було.
`max_gap_ms` — максимум завершених пауз інтервалу та поточного очікування PCM;
незавершене очікування ще не збільшує `gaps_gt_*`.

Для 64-bit counters на 32-bit ESP32 використано спільний `portMUX_TYPE`:
PCM callback має одну коротку critical section зі скалярними операціями і
читанням таймера; задача робить малий snapshot метаданих. Це короткочасно
маскує локальні interrupts і може чекати spinlock іншого ядра. Час цього
впливу на платі ще не виміряний. Під lock немає логів, ділення, allocation,
черг, затримок або доступу до PCM; перший callback логує задача поза lock.

## Збірка та ручний запуск

Перевірено 2026-09-13: `platformio run` — **SUCCESS**, ESP-IDF 6.1.0,
RAM 54244 bytes, flash image code/data 721917 bytes із 1048576 bytes
штатного app-розділу; фізичний flash у конфігурації — 4 МБ.
`git diff --check` і whitespace-перевірка всіх нових файлів пройшли.
Upload/Monitor під час цієї перевірки не запускалися.

Для поточного тесту використовуйте PlatformIO UI, як описано на початку README
і в A/B-протоколі. Якщо підключено кілька COM-пристроїв, оберіть порт ESP32.
CLI використаний агентом тільки для локальних Build і перевірок артефактів;
повторна автоматична перевірка готових збірок — `tools/verify_ab.py` через
PlatformIO Python. Вона не запускає Upload або Monitor.
Сполучіть source з `Faital A2DP Control`, увімкніть безперервний трек і
порівняйте кілька п'ятисекундних вікон за однакових умов з основним застосунком.
Якщо source зберіг стару назву/профілі тієї самої ESP32, видаліть старе
сполучення на source і сполучіть заново.

Скорочений **ілюстративний**, не виміряний UART-лог; MAC і числа умовні:

```text
I (...) CONTROL: CONTROL variant=baseline, build=a2dp_sink_control, device=Faital A2DP Control, ESP-IDF=6.1.0
I (...) CONTROL: Bluetooth Classic controller enabled
I (...) CONTROL: Bluedroid host enabled
I (...) CONTROL: A2DP Sink profile=initialized
I (...) CONTROL: ready: discoverable as Faital A2DP Control; variant=baseline, audio_state=stopped
I (...) CONTROL: connection_state=connected, remote=12:34:56:78:9a:bc
I (...) CONTROL: A2DP audio_mtu=991
I (...) CONTROL: SBC raw=21 15 02 35, sample_rate=44100, channel_mode=joint_stereo, PCM=s16le interleaved, stereo_frame_bytes=4
I (...) CONTROL: audio_state=started
I (...) CONTROL: first PCM callback: timestamp_us=10000000 (reported by stats task)
I (...) CONTROL: stats: elapsed_ms=5000, callbacks=250 (+250), pcm_total=882000, pcm_interval=882000, pcm_rate=176400 B/s, expected_rate=176400, last_size=3528, no_pcm_now_ms=5, max_gap_ms=20, gaps_gt_30ms=0, gaps_gt_50ms=0, gaps_gt_100ms=0, gaps_gt_200ms=0, sample_rate=44100, channel_mode=joint_stereo, audio_state=started
I (...) CONTROL: audio_state=suspended
I (...) CONTROL: connection_state=disconnected, remote=12:34:56:78:9a:bc
I (...) CONTROL: audio_state=stopped (disconnected)
```

## Трактування

- Стабільний `pcm_rate` близько до `expected_rate`, без довгих gaps і майже
  без sequence warnings: підозра на основний застосунок або його конфігурацію.
- Повторні 143000–149000 B/s при 44100 Hz, gaps 100–220 мс та численні
  sequence warnings: дефект відтворюється без нашого аудіопайплайна; перевіряти
  конкретну ESP32, антену/controller або спільну поведінку ESP-IDF/source.
  Сам цей тест ще не відрізняє апаратну причину від проблеми стека/source.
- Зміни sample rate або неочікуваний channel mode зафіксуйте окремо як можливу
  причину. Короткі стартові переходи й навмисні паузи не порівнюйте зі сталим потоком.

Основний застосунок за результатом самої збірки не змінюється: для висновків
потрібен UART-лог контрольного запуску на платі.
