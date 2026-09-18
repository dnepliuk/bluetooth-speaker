# A/B: локалізація втрат Bluetooth PCM

Поточний раунд, 2026-09-17–18: **CPU frequency × Bluetooth modem sleep**,
чотири environments на спільному official-reference коді. Нові фізичні запуски —
**NOT RUN**. Актуальна матриця, аудит і протокол — у розділі 10.

Користувач повідомив historical FAIL для aligned (~141200 B/s) та official
160 MHz / sleep ON (~152000 B/s). Розділи попередніх раундів нижче зберігають
історію реалізації; їхні датовані NOT TESTED не є поточними апаратними статусами.

## Вихідні дані

Результати нижче надані користувачем; під час реалізації A/B не виконувалися
Upload, Monitor або нові вимірювання на ESP32.

| Прошивка | Підтверджений результат |
|---|---|
| Main | 143000–149000 B/s замість 176400, приблизно 17% дефіциту; 20–30 sequence errors/5 s; gaps 100–220 ms; порожній StreamBuffer, іноді overflow/trim |
| Working control | За ~70 s середнє 176267.8 B/s (99.925%); 215–216 callbacks по 4096 bytes/5 s; sequence errors під час playback відсутні; немає gaps >100 ms; типово 30–33 ms, одинично 51–60 ms |

В обох випадках: телефон, 44100 Hz, joint stereo, PCM s16le interleaved L/R,
4 bytes/frame, MTU 991. Для main не зафіксовані I²S errors/short writes,
allocation failures чи недостатні heap/stacks. Дефект main однаковий на
IDF 6.0.1/6.1.0 і залишається без AVRCP/reconnect/RSSI/storage та периферії.
Це показує, що плата і source можуть приймати повний потік у working control,
але ще не визначає конкретну причину різниці.

## Результат аналізу main

Прочитані всі `src/*`, `platformio.ini`, `sdkconfig*` і попередній control.
Джерело локально перенесених параметрів/операцій — commit
`8a36a8c520d775f1a5f54b678254f59728e8cee7`, насамперед main
`app_config.h`, `audio_pipeline.c`, `i2s_output.c`.
Жоден environment не включає main-файли і не залежить від них під час збірки.

Main також містить LED task, перехоплення `vprintf` із пошуком BT fault-рядків,
heap/stack diagnostics, prefetch 8192 bytes і latency trim 28672 → 8192 bytes.
`BT_DIAGNOSTIC_MINIMAL_MODE=1` вимикає лише AVRCP/reconnect/RSSI/MAC tasks;
LED, log hook та pipeline diagnostics при цьому залишаються. З цього переліку
лише prefill 8192 додається до нового clocked variant; решта не переноситься.
Відмінності GAP/SSP initialization і scan
policy теж не перенесені: всі A/B зберігають working control Bluetooth setup.

## Семантичне порівняння sdkconfig

Повна таблиця «параметр / baseline / main / перенесено / причина» знаходиться
у [sdkconfig-comparison.md](sdkconfig-comparison.md). Вона охоплює всі effective
відмінності й 516 перевірених релевантних символів, включно з aliases.
Ці 516 записів є перевірочними даними, а не defaults, що застосовуються до build.

| Параметр / група | Baseline | Main | `main-config` |
|---|---|---|---|
| `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` | 160 | 240 | 240 через `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y` |
| `CONFIG_BTDM_CTRL_MODEM_SLEEP` | y | n | n |
| Controller / Bluedroid core | 0 / 0 | 0 / 0 | Без override |
| BTC / BTU stack | 3072 / 4352 bytes | Так само | Без override |
| Controller BR/EDR ACL / host ACL connections | 2 / 4 | Так само | Без override |
| A2DP / внутрішній SBC, HCI/ACL settings | Увімкнено / штатні | Так само | Без override |
| FreeRTOS tick / cores / SMP options | 100 Hz / 2 / штатні | Так само | Без override |
| `CONFIG_PM_ENABLE` (DFS) | n | n | Без override; runtime PM configuration також не додається |
| Task WDT | enabled, init, 5 s, idle CPU0/1; panic=n | Так само | Без override |
| Compiler optimization | DEBUG (`-Og`), assertions enabled | Так само | Без override |
| Log / BT_APPL / BT_HCI | INFO / WARNING / WARNING | Так само | Без override |
| UART | UART0, 115200 | Так само | Без override |
| Heap poisoning / tracing / SPIRAM | disabled / off / n | Так само | Без override |
| BOYA flash support | n | y | **Не перенесено**, baseline warning збережено |
| Partition table | `partitions_singleapp.csv` | custom `partitions.csv` | **Не перенесено** |

`CONFIG_IDF_INIT_VERSION="6.0.1"` у main — історія створення sdkconfig.
Основний environment закріплює пакет 6.1.0; всі вісім A/B теж використовують
саме `framework-espidf@4.60100.0` і compile-time перевірку IDF 6.1.0.
Попередній `sdkconfig.esp32doit-devkit-v1-idf60` проаналізований окремо;
символи іншої версії не переносяться механічно.

У `sdkconfig.main-config.defaults` є лише **два** canonical overrides.
Частота і sleep перевіряються як один набір конфігурації; якщо він дасть FAIL,
наступний окремий раунд має розділити ці два параметри.

## Environments та коректні A/B-пари

| Environment | UART banner | Еталон порівняння | Єдина змінна / набір, що додається |
|---|---|---|---|
| `control-baseline` | `CONTROL variant=baseline` | Working control до рефакторингу | Лише назва environment і boot/ready banner |
| `control-main-config` | `CONTROL variant=main-config` | `control-baseline` | Набір CPU frequency + modem sleep; той самий drain-only код |
| `control-streambuffer` | `CONTROL variant=streambuffer` | `control-baseline` | StreamBuffer + копіювання + consumer + вимірювання їхнього часу |
| `control-i2s-silence` | `CONTROL variant=i2s-silence` | `control-baseline` | Незалежний I²S/DMA writer цифрової тиші + його статистика |
| `control-pipeline` | `CONTROL variant=pipeline` | `control-streambuffer` | Споживач передає PCM в I²S; додаються DMA, I²S write pacing і write stats |
| `control-clocked-pipeline` | `CONTROL variant=clocked-pipeline` | Failing `control-pipeline` | Постійний I²S writer, receive timeout=0, PCM + тиша, prefill 8192 та counters |
| `control-spsc-pipeline` | `CONTROL variant=spsc-pipeline` | Failing `control-clocked-pipeline` | Статичний SPSC замість StreamBuffer; та сама поведінка writer, додані SPSC counters |
| `control-spsc-dma-aligned` | `CONTROL variant=spsc-dma-aligned` | Failing `control-spsc-pipeline` | Output block 4096 → 3840 bytes; SPSC, prefill 8192, DMA 3×960, producer/writer без інших змін |

Це розгалужена матриця, а не кумулятивний список. У заданому порядку запуску
перехід `main-config → streambuffer` одночасно повертає baseline config і додає
буфер; перехід `streambuffer → i2s-silence` замінює підсистему. **Ці сусідні
запуски не є ізольованими A/B-парами.** Порівнюйте з еталоном у таблиці;
для строгого повтору запускайте відповідний A безпосередньо перед B.
`i2s-silence → pipeline` додатково перевіряє зв'язування source/StreamBuffer з
I²S, але теж містить набір змін, а не тільки одну операцію.

## Збереження baseline й ізоляція build

`sdkconfig.defaults` збережений побайтово. Попередні generated
`sdkconfig.a2dp_sink_control` і його `.pio/build/a2dp_sink_control` не видаляються.
Новий generated `sdkconfig.control-baseline` семантично збігається з working
generated config, включно з BOYA=n.

Текст `pcm_data_callback` має порожній diff із source commit вище, SHA-256
нормалізованого LF-тексту функції:

```text
ee880c9019981988f1e21dd6e488f2149fbf15f70d42efe1a42351442493ba7f
```

Baseline і main-config реєструють саме цю функцію; i2s-silence — також.
Для них немає wrapper, додаткового таймера чи transport call у PCM hot path.
Тільки streambuffer/pipeline/clocked-pipeline/spsc-pipeline/spsc-dma-aligned реєструють `buffered_pcm_data_callback`.
CMake не включає transport/stream/I²S модулі до baseline/main-config; I²S driver
є прямою залежністю тільки i2s-silence/pipeline/clocked-pipeline/spsc-pipeline/spsc-dma-aligned. `MINIMAL_BUILD` збережений.

Перевірено встановлений PlatformIO espressif32 7.1.3
`builder/frameworks/espidf.py`: generated config за замовчуванням —
`sdkconfig.<PIOENV>`, build directory — `.pio/build/<PIOENV>`;
`board_build.cmake_extra_args` справді передається до CMake.
Variant передається як IDF build property через `idf_build_set_property` /
`idf_build_get_property`, щоб він був доступний і в окремому early requirements
проході IDF. Так I²S dependency справді обирається тільки для variants 4/5/6/7/8.
Непідтримувані `sdkconfig_defaults` INI options не використовуються.
Тільки variant 2 додає overlay через штатний CMake `SDKCONFIG_DEFAULTS`.
Решта використовує незмінний `sdkconfig.defaults`.

`build_flags` і CMake задають номер 1..8; header відхиляє відсутній, невідомий
або неузгоджений номер. Одночасного включення кількох variant-ів немає.
Generated configs окремі. Якщо користувач пізніше змінить їх через menuconfig,
defaults автоматично не переб'ють вже збережені значення; verifier виявить
відхилення. Для цього тесту menuconfig і ручне редагування defines не потрібні.

## Перенесений мінімальний тракт

| Параметр | Значення та походження |
|---|---|
| StreamBuffer | 32768 bytes, trigger 4 bytes; `xStreamBufferCreate`, як main |
| Producer | Один A2DP callback; довільний вхідний block без поділу; фактично очікується 4096 bytes |
| Send | Перевірка stereo/вирівнювання; доступне місце округлюється вниз до 4 bytes; `xStreamBufferSend(..., 0)`; ненадісланий хвіст рахується як dropped, без retry |
| Synchronization | Штатний SPSC StreamBuffer, без зовнішнього mutex навколо копіювання; короткий portMUX лише для статистики |
| Consumer/writer | Core 1, priority `configMAX_PRIORITIES - 3` (22), stack 8192 bytes, chunk ≤4096 bytes |
| Streambuffer-only receive | `portMAX_DELAY`, як diagnostic drain-гілка main; прочитане відразу відкидається |
| Pipeline receive | Timeout 100 ms, як main; без prefetch/trim; після suspend споживач відкидає залишки |
| Clocked receive | Тільки timeout=0; chunk завжди 4096, prefill 8192 із передаванням тиші; underflow без повторного prefill |
| I²S | Port 0, master, Philips, stereo s16le, default clock source, auto_clear=true |
| Pins | BCK=26, WS=25, DATA=22, MCLK/DIN unused |
| DMA | 3 descriptors × 960 frames × 4 bytes = 11520 bytes, ~65.3 ms при 44100 Hz |
| I²S IRQ affinity | Init із writer task на core 1; main робить це окремою тимчасовою init task на тому самому core/priority |
| Writes | `i2s_channel_write(..., 1000)`; timeout у **ms**; partial prefix враховується, повторюється лише хвіст; backoff 1 tick тільки при no progress |

I²S-silence постійно пише нульовий 4096-byte chunk із 44100 Hz, навіть під час
паузи source; немає тону, StreamBuffer або доступу до Bluetooth PCM. DMA
визначає темп задачі. Pipeline бере sample rate з SBC, writer змінює I²S clock
лише при зміні частоти; невідомий/mono формат відкидається з діагностикою.
I²S initialization завершується перед Bluetooth startup, як у main, але
не потребує окремої тимчасової задачі. Всі allocations застосунку — під час init;
callback не виділяє і не звільняє пам'ять.

Використано [офіційний I²S API ESP-IDF 6.1](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/peripherals/i2s.html).
Не додано LED, AVRCP application init, reconnect, RSSI, MAC storage, DSP,
latency trim, тестовий тон або BT log interception. Prefill є лише у variants 6/7/8.
Сам IDF, як і раніше,
компілює AVRCP через залежність A2DP, але application CT/TG init відсутня.

## Гіпотеза і реалізація clocked writer

**Гіпотеза, а не доведена причина:**

> Дефект може виникати через зв'язок між producer callback, StreamBuffer wake-up та writer, який по черзі чекає StreamBuffer й блокується в I²S. Clocked writer прибирає blocking wait на StreamBuffer, але залишає I²S blocking write як джерело темпу.

Шлях перевірений безпосередньо за кодом:

```text
main.c: buffered_pcm_data_callback
  → pcm_data_callback (незмінні PCM rates/gaps)
  → control_transport_send → control_stream_send
  → xStreamBufferSend(..., 0)

variant 5: consumer_task
  → control_stream_receive(..., pdMS_TO_TICKS(CONTROL_RECEIVE_TIMEOUT_MS))
  → xStreamBufferReceive(..., timeout), CONTROL_RECEIVE_TIMEOUT_MS=100
  → write_all → control_i2s_write → i2s_channel_write(..., 1000 ms)

variant 6: consumer_task → clocked_loop
  → clocked_read → control_stream_receive(..., 0)
  → xStreamBufferReceive(..., 0)
  → PCM prefix + zero tail, завжди 4096 bytes
  → clocked_write → control_i2s_write → i2s_channel_write(..., 1000 ms)
```

Main має такий самий zero-timeout producer, blocking receive 100 ms після
prefetch і blocking I²S write, але також додаткові функції, перелічені вище.
Variant 5 залишається незмінним failing reference: у ньому немає prefill,
постійного writer тиші чи нового receive. Variant 6 обирається compile-time;
позитивний StreamBuffer timeout і старий `write_all` до нього не компілюються.

У встановленому IDF 6.1 `components/freertos/FreeRTOS-Kernel/stream_buffer.c`
реєструє `xTaskWaitingToReceive` і викликає `xTaskNotifyWait` лише всередині
`if (xTicksToWait != 0)`. Timeout=0 обходить цю гілку. Водночас send/receive
зберігають внутрішні critical sections StreamBuffer, тому нова архітектура
не гарантує усунення всього contention і не доводить причину FAIL.

У clocked steady state є одна спроба receive з timeout=0 за output block.
Немає `vTaskDelay` у `clocked_loop`; швидкість задає блокування I²S write.
Драйвер може спочатку прийняти кілька блоків швидко до заповнення DMA.
Є лише такий самий error backoff 1 tick при write з нульовим прогресом.
Partial write продовжує з фактичного offset, без повтору вже прийнятих bytes;
некратний 4 або завеликий результат I²S зупиняє writer і збільшує errors.
Це аварійна ситуація, а не штатне software pacing.

Після STARTED writer продовжує тишу, а PCM накопичується до 8192 bytes.
Далі він читає до 4096 bytes і нулями заповнює лише відсутній хвіст. Underflow
рахується за кожен неповний block активного playback; він не повертає writer
до prefill. Немає trim, DSP, зміни priority/core/буферів/DMA/BT config.
Частота початково 44100 Hz; як у variant 5, зміна SBC rate застосовується
writer-ом. Під час невдалої reconfiguration передається тиша, retry не частіше
раз на секунду за timestamp, без окремого sleep. Mono/невідомий rate не є
коректним A/B із 44100 Hz stereo і не запускає PCM playback.

### Зміни стану та синхронізація

Producer лишився побайтово тим самим у `main.c` та `control_stream.c`;
`control_transport_send` і `control_transport_callback_done` однакові навіть
після compiler preprocessing variants 5 і 6. Нового gate/mutex/notification
або epoch bookkeeping у PCM callback немає.

Єдиний writer володіє receive, output buffer, carry і I²S. Рідкісні state/format
callbacks variant 6 публікують state + epoch під наявним коротким stats lock.
Writer під цим самим lock бере узгоджений snapshot state/epoch і вже наявних
`queued`/`sends`. Під lock немає receive, копіювання PCM, I²S, logging чи очікування.

На зміні epoch writer скидає prefill/carry і встановлює producer fence за
`sends`. Поки fence активний або source зупинений, він через timeout=0
відкидає лише скінченний prefix до snapshot `queued`. Після наступного
завершеного send і drain до його `queued` fence знімається. Навіть send із
нулем прийнятих bytes публікує `sends`: заповнений буфер не блокує цей перехід.
Оскільки producer один, попередній callback, який міг бути в польоті під час
snapshot, до цієї межі вже завершив копіювання. Нові bytes після cutoff
залишаються в буфері. `xStreamBufferReset` і другий reader не потрібні.

Ціна цього консервативного fence — відкинутий початковий prefix нового PCM
при старті/відновленні, включно щонайменше з одним send після спостереження
нового epoch. Це видно як `discarded_interval`, не producer `dropped_interval`;
це перехідний interval, не steady-state loss. Без нових callbacks fence може
чекати необмежено, але I²S увесь час отримує тишу. Швидкий suspend→start між
ітераціями не губиться, бо epoch змінюється навіть якщо кінцевий state той самий.

Перед кожним I²S write/partial retry перевіряється epoch: при його зміні
непереданий PCM замінюється нулями та рахується як discarded. Уже розпочатий
blocking write і DMA неможливо відкликати без зупинки/очищення I²S. Тому існує
неминучий фізичний хвіст старого PCM: до одного 4096-byte in-flight блока плюс
11520 bytes DMA (номінально ~88.5 ms при 44100 Hz; scheduler/error delays можуть
подовжити перехід). Це не миттєвий mute. Перед новим PCM playback writer
передає щонайменше повну глибину DMA тиші — три повні блоки по 4096 bytes —
та завершує fence/prefill. Старі application bytes не переходять у новий playback.
Фактичний хвіст на DATA pin і pause/reconnect потребують апаратної перевірки.

Нормальний producer надсилає тільки цілі 4-byte stereo frames. Якщо receive
все ж повернув некратний розмір, 1–3 bytes зберігаються в локальному carry,
додаються перед наступним read і ніколи не міняються місцями. В I²S потрапляють
лише цілі frames. `unaligned_reads_interval` показує таку аномалію. При epoch
change carry відкидається з обліком discarded; drain до published producer
boundary повертає reader до межі frame. Усі буфери виділені при init або в
8192-byte writer stack поза callback; додаткової черги PCM немає.

### Нові поля clocked

До п'ятисекундної групи додається `CONTROL: clocked:`. Snapshot і timestamp
спільні з transport/I²S; початковий/післяпаузовий interval позначається
`full_started_interval=0`. Під час pause без STARTED основний stats task не
друкує групи, але writer продовжує тишу; наступний interval включає цю тишу.

| Поле | Семантика |
|---|---|
| `prefill_active` | Останній опублікований стан writer, включно з очікуванням fence/стартом |
| `prefill_completions` | Загальна кількість завершень prefill від boot |
| `pcm_played_interval` | PCM bytes, прийняті I²S API, не вимірювання DATA pin |
| `silence_inserted_interval` | Нульові bytes, прийняті I²S API, включно зі startup/pause/prefill |
| `silence_only_blocks_interval` | Завершені output blocks без прийнятого PCM |
| `partial_pcm_blocks_interval` | Завершені blocks із 1–4095 PCM bytes (фактично кратно 4) |
| `full_pcm_blocks_interval` | Завершені blocks із 4096 PCM bytes |
| `underflow_events_interval` | Неповні blocks під час активного playback після prefill; кожен block, не лише початок серії |
| `stream_read_us_avg` | Середній час receive за interval, включно з transition drain і порожніми reads |
| `stream_read_us_max` | Максимум часу receive від boot |
| `unaligned_reads_interval` | Кількість некратних 4 результатів receive за interval; очікується 0 |

`pcm_played_interval + silence_inserted_interval == i2s_written_interval`
для одного snapshot, включно з partial writes. Block counters публікуються
після повного write, тому snapshot між partial writes може не мати кратності
4096 за цими byte counters. `consumed` рахує і transition drain, і PCM/carry;
`discarded` відрізняє навмисне очищення від producer drops. Старий
`read_timeouts_interval` збережений, але у clocked дорівнює 0: timeout wait
відсутній, starvation показує `underflow_events_interval`.

## Семантика статистики

Незмінний `CONTROL: stats:` містить PCM rates/gaps/audio state кожні ≥5 s
активного потоку. Поруч, у ту саму reporting-групу, додаються `transport:`
і/або `i2s:`; окремого log task або пакетних логів немає.

- `pcm_rate` вимірює всі bytes на вході callback, до можливих drops.
  `expected_rate=sample_rate*2*2`; при 44100 Hz це 176400 B/s.
- `queued_interval`, `consumed_interval`, `dropped_interval` означають відповідно
  прийняті StreamBuffer bytes, прочитані consumer bytes та відкинуті producer bytes.
  `invalid_interval` — підмножина dropped через невідповідний PCM формат.
  `discarded_interval` — прочитані/незаписані залишки при suspend або помилці clock.
- `buffer_fill` — поточний sampled fill. `buffer_min/max` і максимальні durations
  накопичуються від boot; середні durations та byte/error counters — за interval.
  Без prefetch `buffer_min=0` очікуваний і сам по собі **не є FAIL**.
  Watermark може пропустити миттєвий пік після send, якщо consumer вже прокинувся;
  це та сама властивість sampling, що в main. `read_timeouts_interval` у pipeline
  рахує невдале очікування 100 ms під час STARTED.
- `callback_total_us` починається першою операцією buffered callback, до
  baseline stats lock; кінцева мітка береться після baseline stats, StreamBuffer
  API, запитів fill і звільнення transport stats lock. Таким чином враховано
  очікування обох locks, копіювання, пробудження/preemption consumer і всю роботу
  callback перед публікацією timing. Після кінцевої мітки залишаються лише
  фіксовані scalar/lock-free atomic stores самого вимірювання та function epilogue;
  їх неможливо включити у власний вже опублікований timestamp. Нових contended
  locks після цієї мітки немає. Це wall time, не тільки CPU execution time.
- Timing публікується одним producer через sequence і 32-bit atomics; 64-bit sum
  читається без розривів. Повтор snapshot можливий тільки в low-priority stats task.
  `stream_send_us` охоплює query доступного місця, вирівнювання і zero-timeout send;
  решта callback врахована в `callback_total_us`.
- `i2s_written_interval` рахує bytes, прийняті write API, включно з partial prefix;
  це не вимірювання фізичного DATA pin. `i2s_rate` використовує справжній elapsed
  time; write operations/errors/short writes — приріст за цей interval.
- PCM і transport snapshots беруться окремо, тому `elapsed_ms` може трохи
  відрізнятися; кожна rate використовує власний timestamp. Перший transport
  interval включає startup; після pause/config change він також перехідний.
  Для висновків беріть `full_started_interval=1` і стабільний PCM формат.
  Різниця одного 4096-byte блока між сусідніми рядками/вікнами сама по собі
  не доводить втрату: callback, transport і завершення I²S write мають різні межі.
  У baseline/main-config додаткового transport-рядка немає: відкиньте перший
  PCM stats interval після STARTED; наступні до pause/зміни формату є повними.
- Штатні BT_APPL/BT_HCI warnings не приховуються і не перехоплюються. Allocation
  errors під час init перевіряються; нового heap-hook у baseline не додається.

## Ручний тест у VS Code

1. **File → New Window**, потім **File → Open Folder**:
   `C:\bluetooth-speaker\control\a2dp_sink_control`.
2. Відкрити PlatformIO sidebar → **Project Tasks**. Усі шість environments
   мають бути видимі; за потреби оновити список Project Tasks.
3. Для потрібного environment: **General → Build**, потім **General → Upload**.
4. Відкрити **Platform → Monitor** на 115200. Натиснути **EN/RESET**, щоб
   захопити boot banner. Перед кожним тестом звірити `CONTROL variant=...`.
5. Підключити телефон до **Faital A2DP Control**. Wi-Fi і hotspot телефона
   вимкнути; використати той самий завантажений трек; відстань 20–30 см.
6. Програвати 60–90 s без pause/seek. Зберегти boot, SBC, audio-state та
   щонайменше 12 послідовних п'ятисекундних `stats:` груп із сусідніми warnings.
   Бажано 90 s: це залишить ≥12 повних інтервалів після стартового переходу.
7. Закрити Monitor перед наступним Upload. Повторювати з тією самою платою,
   USB-живленням, телефоном, треком і положенням.

Перші п'ять тестів уже виконані користувачем. Наступний тест у UI:
**control-clocked-pipeline → General → Upload → Platform → Monitor (115200)**.
Після EN/RESET має бути `CONTROL variant=clocked-pipeline`.
Для строгого повтору порівняйте `control-pipeline → control-clocked-pipeline`
в однакових умовах. При FAIL повторіть A/B-пару, включно з еталоном;
якщо сам baseline тепер нестабільний, порівняння інших variants не є доказовим.

Під час усіх тестів підключені тільки ESP32 і USB. DAC, підсилювач, LM2596 та
динаміки від'єднані; I²S pins можуть працювати без фізичного DAC. PowerShell
Upload або ручне перемикання `#define` користувачу не потрібні.

Після основного безперервного вимірювання окремо перевірте pause/resume та
швидкий disconnect/reconnect. Очікуються новий prefill, приріст
`prefill_completions`, transition `discarded_interval` і тиша під час очищення;
ці інтервали не домішуйте до steady-state rate. Збережіть усі відповідні
audio-state рядки. Відсутність старого звуку/фізичний DMA tail не можна
підтвердити самою локальною збіркою або лічильниками API.

## PASS / FAIL

PASS: зважена середня PCM rate у межах ±1% expected rate; немає регулярних
sequence errors, gaps >100 ms, прогресивного дефіциту чи allocation failures;
у transport немає dropped bytes, I²S errors/short writes. Для 44100 Hz межі
PCM rate — **174636–178164 B/s**. Рахуйте `sum(pcm_interval)/sum(elapsed_ms/1000)`
за повними інтервалами, а не просте середнє округлених rates.

Для clocked додатково: I²S rate також ±1%, callback/send maxima не повертаються
до багатомілісекундних затримок; після prefill переважають full PCM blocks,
silence insertion не становить постійної значної частки потоку.

FAIL: повторні 143–149 kB/s, регулярні sequence errors, gaps
100–266 ms, fill регулярно 0 і 32768, dropped bytes/catch-up,
callback/send max знову 2–3 ms або I²S errors/
short writes. Правильний PCM rate разом із dropped/write errors означає
проблему після callback; це не доказ втрати Bluetooth PCM до callback.

Одиничні gaps 30–60 ms при правильній середній rate й відсутніх sequence
errors не є FAIL. Старт, pause, seek, disconnect, mono та зміна sample rate
оцінюються окремо. Порожній буфер drain-consumer без timeouts/drops — нормальний.

## Таблиця фактичних результатів

Підтверджені результати 1–9 надані користувачем у запитах. Сирі UART-файли,
точні дата/source/трек і кількість повних інтервалів до цього запиту не додані;
значення нижче не є новими вимірюваннями агента. SUCCESS збірки не є PASS.

| Environment | PCM B/s | Sequence / HCI errors | Gaps >100 ms | Callback/send max us | I²S B/s; errors/shorts | Результат |
|---|---|---|---|---|---|---|
| control-baseline | 176299.9 (99.943%) | 0 / не повідомлено | Немає | n/a | n/a | PASS |
| control-main-config | 176264.5 | 0 / не повідомлено | Немає | n/a | n/a | PASS; CPU 240, sleep off |
| control-streambuffer | 176274.5 | 0 / не повідомлено | Немає | 449 / 294 | n/a | PASS; dropped=0 |
| control-i2s-silence | 176218.8 | 0 / не повідомлено | Немає | n/a | 176394; 0/0 | PASS |
| control-pipeline | 145947.4 (82.737%), без фінального catch-up | ~215 / 1 reassembly | Регулярно 100–266 ms | 2558 / 2458 | Rate не надано; 0/0 | FAIL; fill 0–32768, drops |
| control-clocked-pipeline | 141125.9 (80.0%) | 175 / не повідомлено | Регулярно ~280–304 ms | ~2253 / ~2146 | ~176400; 0/0 | FAIL; fill 0–32768, drops ~40–65 KiB/5 s, silence ~229–250 KiB/5 s |
| control-spsc-pipeline | ~151296 | 172 / не повідомлено | Регулярно 270–291 ms | Callback max не надано; producer переважно 20–53 | ~176400; 0/0 | FAIL; drops ~86–131 KiB/5 s, silence ~229–246 KiB/5 s |
| control-spsc-dma-aligned | ~141200 | Sequence ~кожні 350 ms / HCI не повідомлено | Не повідомлено | Не повідомлено | Rate не надано; 0/0 | **Historical FAIL**; короткий catch-up не є PASS |
| control-official-i2s-reference, 160/ON | ~152000 | Sequence ~кожні 290 ms / HCI не повідомлено | Не повідомлено | Не повідомлено | Rate не надано; 0/0 | **Historical FAIL**; underflows/application drops, короткий catch-up не є PASS |

У SPSC consumer переважно 12–16 us, maximum 37 us. Заміна StreamBuffer
не усунула дефект; його причина не обмежується StreamBuffer. Наведені дані
SPSC отримані зі зведення користувача, а не з нового вимірювання агента.

Фінальні `pcm_rate=176467 B/s`, `buffer_fill=28672`, `dropped_interval=8192`
у pipeline означають catch-up сплеск; це не відновлення стабільного потоку.
Окремі StreamBuffer та I²S тести проходять, а їх поєднання відтворює дефект
main. Contention/backpressure/scheduling interaction — напрям перевірки;
конкретний механізм ще не доведений.

Повернути для кожного environment повний текст UART від RESET: banner/IDF,
transport/I²S parameters, connection MAC/MTU, raw SBC, частоту/channel mode,
audio states, first PCM, ≥12 stats groups та **всі** BT_APPL/BT_HCI warnings,
init errors або watchdog/panic/reset повідомлення. Додати телефон/ОС,
назву й тривалість треку, відстань, стан Wi-Fi/hotspot та власні pause/seek.

## Історична локальна перевірка додавання clocked variant

Файли цього раунду (відносно `control/a2dp_sink_control`): `platformio.ini`,
`CMakeLists.txt`, `src/CMakeLists.txt`, `src/control_variant.h`,
`src/control_audio_config.h`, `src/control_transport.c`, `README.md`,
`docs/ab-investigation.md`, `docs/clocked-reference.json`,
`docs/verification.json`, `tools/verify_ab.py`, `tools/verify_clocked.py`.
Збережені попередні незакомічені зміни A/B-матриці; `src/main.c`, stream/I²S
модулі control і baseline defaults у цьому раунді не редагувалися.

Перевірено 2026-09-15, PlatformIO espressif32 7.1.3 / ESP-IDF 6.1.0:

| Environment | Build | Static RAM bytes | App flash bytes | Config / ELF / isolation checks |
|---|---|---|---|---|
| control-baseline | SUCCESS | 54244 | 721949 | PASS |
| control-main-config | SUCCESS | 54244 | 721157 | PASS |
| control-streambuffer | SUCCESS | 54596 | 728077 | PASS |
| control-i2s-silence | SUCCESS | 54848 | 751069 | PASS |
| control-pipeline | SUCCESS | 54912 | 756957 | PASS |
| control-clocked-pipeline | SUCCESS | 55088 | 759849 | PASS |

Static RAM не включає виділені під час init StreamBuffer, task stacks і DMA.
Усі п'ять baseline-config variants мають однаковий semantic SHA-256 config:
`1461cdd3bda5b5a5fb65de4ec3222ba57b344424999701a10469d499c2738113`.
Основні 26 файлів незмінні; SHA-256 маніфесту їхніх шляхів і початкових хешів:
`5a176b3286998d42d8d45712b8f4459d5994797174b58fb472bb1733f24182c1`.
`git diff --check` та whitespace checks нових файлів пройшли.

`tools/verify_ab.py` перевіряє 26 SHA-256 основних файлів із `docs/reference.json`,
незмінність baseline callback/defaults, відповідність generated configs,
окремі config paths, ELF banner literals і відсутність StreamBuffer/I²S/AVRCP
application init symbols там, де їх не повинно бути. Також виконує позитивні
й негативні compile checks selector header. Працює лише з готовими збірками,
не запускає Build/Upload/Monitor; результат із build-метаданими збережено у
`docs/verification.json`.
Для clocked доданий `tools/verify_clocked.py`, який використовує реальний GCC
preprocessor із compile database. `docs/clocked-reference.json` знятий **до**
додавання variant 6: SHA-256 усіх 12 application translation units попередніх
п'яти environments мають збігатися після preprocessing. Окремо перевіряються
незмінні main/stream/I²S source-файли control, однакові producer/timing функції
variants 5/6, єдиний активний receive call із timeout=0, відсутність delay у
steady loop, лише error backoff у write, та I²S timeout 1000 ms. ELF checks
підтверджують наявність `clocked_loop` тільки у variant 6 і правильні banners.
Попередні конфіги та діагностичні документи збережені.

На момент додавання clocked були непідтверджені: чи саме StreamBuffer wake-up
спричиняє дефект; чи clocked усуває sequence errors/gaps; фактичні I²S clocks,
частка inserted silence, timing maxima, вплив додаткових counters та поведінка
pause/швидкого reconnect на ESP32. Нестандартні SBC rates/mono не перевірені.
Програмна гарантія fence спирається на штатний контракт одного послідовного
PCM producer; вона не маркує приховані пакети всередині Bluetooth stack.
Встановлений IDF при suspend задає rx_flush і запитує очищення media queue,
але фізичний результат цього переходу має підтвердити UART/апаратний тест.

Build/preprocessing не перевіряють реальний scheduler, RF, DMA pin timing чи
асинхронні переходи на платі. Наступний отриманий clocked FAIL і реалізація
окремого SPSC A/B описані нижче. Основна прошивка залишається незмінною.

## Сьомий тест: control-spsc-pipeline — реалізація 2026-09-15

Цей розділ зберігає історію підготовки SPSC. Отриманий 2026-09-16 фізичний
результат — **FAIL**, наведений у таблиці вище; наступний A/B описаний нижче.

Отриманий від користувача результат clocked: PCM 141125.9 B/s (80.0%),
175 sequence errors, регулярні gaps ~280–304 ms при стабільному I²S ~176400 B/s,
errors/shorts=0. Тиша ~229–250 KiB/5 s, drops ~40–65 KiB/5 s, fill 0–32768,
stream_send_us_max ~2146, callback_total_us_max ~2253. Це спростовує пояснення
дефекту **лише** blocking receive 100 ms. Попередні числові результати тестів
1–5 вище збережені; значення ~176268 B/s у новому запиті відповідає окремому
working-control вимірюванню 176267.8 B/s у вступі.

```text
незмінний buffered_pcm_data_callback
  → control_transport_send → control_spsc_send
  → static internal DRAM, 32768 bytes
  → clocked_read → control_spsc_receive (без timeout і без очікування)
  → той самий clocked_loop / clocked_write → I²S
```

### Ізоляція та memory ordering

CMake додає `control_spsc.c` лише для selector 7 та не додає `control_stream.c`.
Історичний `CONTROL_HAS_STREAM` означає використання buffered PCM callback;
для variant 7 backend обирає `CONTROL_SPSC`. Завдяки цьому `src/main.c` разом
із реєстрацією PCM callback, SBC, Bluetooth states і `stats` не редагувався.
Локальні compile-time aliases у transport замінюють тільки buffer API.

Усі effective sdkconfig settings збігаються з clocked: CPU 160 MHz, modem sleep
увімкнено, BT/controller, FreeRTOS, compiler, logging, без PSRAM. Збережені
I²S GPIO26/25/22, Philips s16le stereo, 44100 Hz, DMA 3×960, writer core 1,
priority 22, stack 8192, chunk 4096, prefill 8192. `control_i2s.c/.h`,
`control_audio_config.h`, callback source та defaults не змінені.

Кільце — `DRAM_ATTR` byte array, вирівняний до 4 bytes, та два природно
вирівняні `uint32_t`. Producer пише тільки write counter, consumer — тільки
read counter. Свою позицію читають relaxed, позицію іншого власника — acquire.
Release write відбувається після обох `memcpy`; consumer бачить завершені
PCM bytes. Release read виконується після завершення читання; producer не
перезаписує ще не скопійовані consumer-ом дані. Копіювання має одну або дві
частини; немає retry loop, allocation, mutex/queue/semaphore/StreamBuffer,
notifications або logging у модулі кільця.

Використані [GCC `__atomic` builtins](https://gcc.gnu.org/onlinedocs/gcc/_005f_005fatomic-Builtins.html).
Встановлений `xtensa-esp32-elf-gcc` 15.2.0 / ESP-IDF 6.1.0 перевірений окремим
assembly probe та production object: acquire/release генерують load/store і
`memw`; undefined symbols кільця — тільки `memcpy` та `esp_timer_get_time`.
`__atomic_always_lock_free(4, 0)` перевірений compile-time. 64-bit atomic API
не використовується: встановлений `esp_libc/src/stdatomic.c` емулює його lock-ом.
Додані assertions capacity=32768, power-of-two, unsigned distance, frame=4,
chunk=4096, prefill=8192 і незмінних writer settings.

### Переповнення, drops і flush

`(uint32_t)(write - read)` завжди в межах 0..32768. Обидва counters просуваються
на кратне 4; переповнення modulo 2³² визначене для unsigned. Маска 32767
вибирає позицію в кільці; 32768 ділить 2³², тому фізична позиція зберігається
після rollover. У кільці доступні всі 32768 bytes, sentinel не потрібен.

Як у StreamBuffer reference: NULL/нульовий/некратний 4 блок не приймається;
некратний блок відкидається **цілком**, щоб не зсунути L/R. Для коректного
блока приймається найбільший доступний aligned prefix, решта входить у
`dropped_interval`. `overflow_events` збільшується лише для коректних блоків,
яким не вистачило місця; invalid bytes рахуються окремо.

Flush виконує незмінний `clocked_drain`: consumer читає й відкидає скінченний
опублікований префікс до snapshot `queued`. Він не змінює write; producer не
змінює read. Немає reset обох counters, навіть у `control_spsc_init` (це no-op,
початкові нулі задаються static initialization). Повторний producer після
snapshot може безпечно писати у звільнені байти; дані за cutoff залишаються.

Збережений epoch/fence reference: на pause/stop/reconnect/format change writer
починає новий prefill і відкидає старе PCM. Завершений наступний send, навіть
із sent=0, засвідчує завершення попереднього producer, який міг бути in-flight
у момент snapshot. Його опубліковані bytes теж відкидаються до зняття fence.
Це навмисно може відкинути стартовий префікс нового сегмента. Виводиться тиша
щонайменше на DMA depth 11520 bytes (три output blocks), потім prefill 8192.
Underflow не запускає prefill повторно; відсутній хвіст 4096-byte output
заповнюється нулями. PCM уже всередині активного I²S write/DMA неможливо
відкликати; приховані пакети Bluetooth стеку не мають міток epoch застосунку.

### Діагностика та межі неблокуючої реалізації

Разом із незмінними `stats`, `i2s` і `clocked` друкується один `SPSC`-рядок
раз на ≥5 s у STARTED. Він містить elapsed, queued/consumed/dropped, overflow,
fill/capacity, min/max, producer/consumer avg/max us і wrap_writes/reads.
Додатково збережені full_started_interval, invalid/discarded і callback timing.
`clocked` зберігає PCM played, inserted silence, silence-only/partial/full blocks,
underflows, prefill і read timing. Історичні назви `stream_read_us_*` у цьому
рядку означають час читання обраного backend, у variant 7 — SPSC.

Bytes/events/wraps та timing averages — за інтервал; maxima, watermarks і
prefill_completions — lifetime, як у reference. `consumed` включає flush,
а `pcm_played` — лише PCM, прийняте I²S API. `discarded` не є overflow.
Watermarks і fill є samples власників; stats task не читає пару read/write
самостійно, бо третій читач міг би отримати неузгоджені позиції різних моментів.
Wrap counters — окремі 32-bit atomic load/store з одним власником; вони
рахують двочастинні копіювання, а не точне потрапляння в кінець масиву.
Їх snapshots можуть відрізнятися на одну поточну операцію від transport snapshot.

Саме кільце не чекає, не блокує scheduler і не має critical sections. Увесь
PCM callback **не** називається wait-free: для A/B збережені наявні короткі
`s_stats_lock`/`s_lock` зі скалярною статистикою та state/fence snapshots.
Вони існували в clocked; memcpy, logging і I²S виконуються поза ними. Нові
overflow/fill counters лише додають bounded scalar updates. Прибирання цієї
наявної синхронізації було б окремою змінною наступного A/B.

### Перевірки сьомого variant

Початковий `tools/verify_ab.py` пройшов **до** змін. Новий
`docs/spsc-reference.json` тоді ж зафіксував 16 application translation units
шести environments і allocated ELF sections. Старі reference JSON не
перегенеровуються. `verify_ab.py` викликає старі `verify_clocked.py` checks та
нові `verify_spsc.py`: порівнюються ті самі 16 hashes, попередні ELF sections,
main/callback і I²S source, clocked writer після підстановки backend, повні
sdkconfig semantics, selector 1..7, banners, source lists та symbols.

Важливе уточнення preprocessing: встановлений
`freertos/esp_additions/include/freertos/idf_additions.h:23` безумовно включає
`stream_buffer.h`. Тому його **декларації** присутні навіть у baseline/main.
Їх не приховуємо й SDK не патчимо. Verifier перевіряє відсутність **викликів**
StreamBuffer у preprocessed application functions та будь-яких відповідних
symbols у SPSC ELF; у самому ring translation unit немає навіть декларацій.

`tools/verify_spsc_runtime.py` компілює `tools/test_spsc.c`, який включає
production `control_spsc.c`. Windows-host stubs замінюють лише IDF attributes,
тип error і timer. Перевіряються full/empty, invalid/alignment, частковий
prefix, FIFO byte order, фізичний wrap і UINT32 rollover, 100000 випадкових
операцій проти незалежного плоского FIFO, призупинений між copy/publication
producer під час discard та 8000000 frames між двома threads. Assertions
примусово ввімкнені; test compile відхиляє NDEBUG. Це не емуляція ESP32/RF/DMA.
Portable Zig 0.14.1 зберігається лише у ignored `.pio/host-tools`, не є
залежністю firmware. Повторний запуск за наявності цього компілятора:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" tools/verify_ab.py
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" tools/verify_spsc.py
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" tools/verify_spsc_runtime.py --zig .pio/host-tools/ziglang/zig.exe
git diff --check
```

Поточні build sizes і фактичні результати всіх локальних checks збережені в
`verification.json`. Повні firmware images можуть відрізнятися через build
date/time та ELF hash у `.flash.appdesc`; усі інші allocated ELF sections
попередніх variants мають збігатися зі snapshot.

### Ручний тест і висновок

Точні кроки **VS Code PlatformIO UI** наведені в
[README: ручний SPSC-тест](../README.md#ручний-spsc-тест-у-vs-code): окреме вікно
проєкту → `control-spsc-pipeline` → General Build → Upload → Platform Monitor
115200 → EN/RESET → banner → телефон `Faital A2DP Control`.
Той самий локальний трек, Wi-Fi/hotspot off, 20–30 см, 60–90 s і ≥12 повних
груп `stats` / `SPSC` / `i2s` / `clocked` разом з усіма BT warnings.

Критерії PASS не змінені: зважена PCM rate ±1% (174636–178164 B/s), без
регулярних sequence errors/gaps >100 ms, без постійних drops після prefill,
inserted silence близька до нуля, I²S ~176400 B/s, errors/shorts=0. Стартові
та pause/reconnect вікна оцінюються окремо. Новий variant **не має фізичного
PASS**; Upload/Monitor агент не запускає.

SPSC PASS істотно підтримає гіпотезу проблемної взаємодії StreamBuffer з
producer/consumer та I²S. SPSC FAIL означатиме, що причина не обмежується
StreamBuffer і наступний напрям — Bluetooth/I²S scheduling та interrupts.
Основна прошивка не змінюється до фізичного підтвердження.

### Фактичний результат локальної перевірки SPSC (2026-09-15)

| Environment | Build | Static RAM bytes | App flash bytes | Config / source / isolation |
|---|---|---|---|---|
| control-baseline | SUCCESS | 54244 | 721949 | PASS |
| control-main-config | SUCCESS | 54244 | 721157 | PASS |
| control-streambuffer | SUCCESS | 54596 | 728077 | PASS |
| control-i2s-silence | SUCCESS | 54848 | 751069 | PASS |
| control-pipeline | SUCCESS | 54912 | 756957 | PASS |
| control-clocked-pipeline | SUCCESS | 55088 | 759233 | PASS |
| control-spsc-pipeline | SUCCESS | 87904 | 790133 | PASS |

SPSC array знаходиться у внутрішній DRAM за `0x3ffbe72c`, size=32768,
alignment=4. У SPSC ця пам'ять входить до static RAM; reference виділяє кільце
динамічно під час init, тому пряме порівняння лише static RAM оманливе.
`DRAM_ATTR` розміщує нульовий array у DRAM data section, і ці байти також
входять до flash image. Немає залежності від PSRAM.

**PASS:** початковий verifier перед змінами; всі сім Build; незмінність 26
root-файлів і 16 preprocessed application units попередніх шести variants;
старий `verify_clocked.check_clocked`; SPSC code/isolation/config/banner/
atomic-assembly checks; host C runtime tests; `git diff --check`.

**FAIL, не приховано:** додаткова строга перевірка allocated ELF sections
проти snapshot до SPSC не проходить для `control-i2s-silence` (6 секцій)
і `control-clocked-pipeline` (4 секції). Це більше, ніж build timestamp.
Тому `tools/verify_ab.py` і `tools/verify_spsc.py` виводять повний звіт із
`checks=FAIL` для binary comparison та завершуються exit 1. Незмінні
preprocessed hashes і успішний Build не замінюють цей критерій.

Під час першої повної збірки також відрізнявся main-config. Для діагностики
його ELF/map збережені в `.pio/binary-audit`, після чого без змін source або
config виконані relink (bytes стабільні) і clean rebuild (bytes змінилися).
У двох linker maps список LOAD починався відповідно так:

```text
до clean rebuild:    main.c.o, esp_driver_spi, bt, __pio_env, xtensa
після clean rebuild: main.c.o, __pio_env, esp_driver_spi, xtensa, bt
```

У незмінному main-config змінилися `.iram0.vectors`, `.iram0.text`,
`.dram0.data`, `.flash.rodata`, `.flash.tdata`, `.flash.text`; після цієї
контрольної clean build він знову збігається з початковим snapshot. Це прямий
доказ невідтворюваного порядку лінкування незалежно від редагування SPSC,
але не доказ, що кожен байт відмінності інших environments пояснений ним.
Зокрема новий clocked flash size 759233 відрізняється від історичного 759849;
історичну таблицю вище не переписано. Код clocked після preprocessing
ідентичний початковому, включно з writer і producer.

Не змінювали SDK, linker flags або параметри A/B заради отримання потрібного
хешу; strict reference не перегенеровували. Гарантія цього раунду — незмінний
попередній application code/config і основні 26 файлів, **не** побайтова
незмінність усіх готових images. Повний audit з expected/actual hashes,
окремими PASS/FAIL, build sizes і результатом host tests — `verification.json`.

Файли саме цього раунду: `platformio.ini`, `CMakeLists.txt`,
`src/CMakeLists.txt`, `src/control_variant.h`, `src/control_transport.c`,
нові `src/control_spsc.c/.h`, `README.md`, `docs/ab-investigation.md`,
`docs/spsc-reference.json`, `docs/verification.json`, `tools/verify_ab.py`,
нові `tools/verify_spsc.py`, `tools/verify_spsc_runtime.py`, `tools/test_spsc.c`.
Попередні незакомічені зміни залишені; root application, `src/main.c`,
`control_stream.c/.h`, `control_i2s.c/.h`, `control_audio_config.h`,
`control_transport.h`, старі reference JSON і `verify_clocked.py` не редагувалися.

## Восьмий тест: control-spsc-dma-aligned (2026-09-16)

### Підстава та межі гіпотези

Користувач повідомив FAIL SPSC: ~151296 B/s, 172 sequence errors, gaps
270–291 ms, producer переважно 20–53 us, consumer 12–16 us (max 37 us).
I²S ~176400 B/s, errors/shorts=0, але drops ~86–131 KiB/5 s і inserted
silence ~229–246 KiB/5 s. Сам по собі StreamBuffer не пояснює дефект,
оскільки той відтворюється без нього.

У попередньому writer output=4096 bytes, descriptor=960×4=3840 bytes.
Найменший спільний обсяг — `15×4096 = 16×3840 = 61440 bytes`,
`61440/176400 = 0.3482993 s`. За описом користувача sequence errors
виникали переважно кожні 0.34–0.35 s, а `recv` часто зростав на `0x0F`.
Це кореляція, яка мотивує A/B, а не доказ причинності. Сирий UART-лог
до цього запиту не доданий; числа не видаються за власне вимірювання агента.

### Точний diff конфігурації

| Параметр | control-spsc-pipeline | control-spsc-dma-aligned |
|---|---|---|
| Selector / banner | 7 / spsc-pipeline | 8 / spsc-dma-aligned |
| **CONTROL_CHUNK_BYTES / output buffer** | **4096** | **3840** |
| SPSC capacity / stereo frame | 32768 / 4 bytes | 32768 / 4 bytes |
| Prefill / re-prefill on underflow | 8192 / no | 8192 / no |
| DMA descriptors × frames | 3 × 960 | 3 × 960 |
| DMA descriptor / depth | 3840 / 11520 bytes | 3840 / 11520 bytes |
| Writer priority / core / stack | 22 / 1 / 8192 | 22 / 1 / 8192 |
| Rate / expected bytes per second | 44100 / 176400 | 44100 / 176400 |
| BCK / WS / DATA | 26 / 25 / 22 | 26 / 25 / 22 |
| I²S PCM / timeout / pacing | Philips s16le stereo / 1000 ms / blocking I²S | Ідентично |
| CPU / BT modem sleep | 160 MHz / enabled | Ідентично |
| SDK, controller, FreeRTOS, logging, pairing | Reference settings | Ідентично |

Selector/name та одноразова boot-діагностика ідентифікують тест. Єдина
функціональна відмінність — розмір output block. Немає нового sdkconfig
overlay, compile flags для оптимізації, зміни DMA або прихованої staging
копії 4096 bytes. У `control_audio_config.h` тільки selector 8 отримує 3840;
усі попередні variants, включно із SPSC, зберігають 4096.

Збережений той самий `clocked_loop`: неблокуюче читання до `sizeof(chunk)`,
PCM prefix + zero tail, pacing через `clocked_write`. Локальний
`uint8_t chunk[3840U]`; існуючий 4-byte carry не є staging block 4096.
Жодної нової затримки немає; steady-state loop не викликає `vTaskDelay`.
Кожен штатний I²S call передає 3840 bytes. Наявна error recovery дописує
ненаписаний хвіст після short write і має backoff 1 tick лише при нульовому
прогресі. Вона не змінюється заради буквальних 3840 при аварійному retry,
бо це змінило б порядок/повтор PCM і додало другу функціональну змінну.
Errors/shorts мають дорівнювати нулю для фізичного PASS.

Prefill залишається **8192**, хоча це не кратне output=3840. Underflow
не запускає його повторно. Epoch/fence та consumer-owned discard не змінені.
Умова початкової тиші залишається «не менше DMA depth 11520 bytes»;
зміна chunk природно дає 3×3840=11520 замість 3×4096=12288 bytes тиші.
Це наслідок того самого алгоритму, а не окреме налаштування.

У ring module додано окрему гілку compile-time assertion для selector 8:
output=3840, output%4=0, output≤capacity і output=DMA frames×frame bytes.
Попередня assertion output=4096 лишається активною для selector 7. Assertions
capacity=32768, power-of-two, frame=4, prefill=8192/alignment, lock-free 32-bit
atomics, writer priority/core/stack збережені.

### Boot log і очікувана статистика

```text
CONTROL variant=spsc-dma-aligned
clocked writer: receive_timeout=0, prefill_bytes=8192, output_bytes=3840, pacing=blocking-I2S, re_prefill_on_underflow=no
writer DMA alignment: output_bytes=3840, dma_descriptor_bytes=3840, dma_frames=960, frame_bytes=4, writer_dma_aligned=yes
```

Додано лише один boot log. `stats`, `SPSC`, `i2s`, `clocked`, їх counters,
рівні логування та інтервал ≥5 s залишаються ідентичними. При 44100 Hz:
`176400×5/3840 = 229.6875 writes`, очікувано 229–230 за повні п'ять секунд;
`3840/176400×1000 = 21.7687 ms/write`. Реальний `elapsed_ms` може трохи
перевищувати 5000, тому нормуйте очікувану кількість writes до тривалості
вікна. Перший інтервал містить init/prefill і оцінюється окремо.

### Регресійна й host-перевірка

До редагування збережено `docs/aligned-reference.json`: **20 preprocessed
application translation units**, усі сім повних semantic sdkconfig hashes
та allocated ELF sections. Попередні reference JSON залишені без змін.
ELF/map до змін також скопійовані у ignored `.pio/aligned-audit`, щоб
дослідження binary differences не спиралося лише на припущення.

Новий `tools/verify_aligned.py` перевіряє:

- незмінність усіх 20 попередніх preprocessed одиниць і семи sdkconfigs;
- однаковий повний sdkconfig у variants 7/8;
- compiler assertions для output 4096/3840, prefill 8192, DMA 3×960,
  capacity/frame, priority 22/core 1/stack 8192, rate і write timeout;
- однакові main/Bluetooth/PCM callbacks, за винятком variant literal;
- ідентичний I²S translation unit, незмінний producer/consumer ring;
- ідентичний **увесь** preprocessed transport після підстановки
  3840→4096 та вилучення лише одного нового boot log;
- однакові loadable code/data sections SPSC та I²S **object files** між
  variants 7/8, незалежно від адрес фінального лінкування;
- output buffer 3840, незмінні prefill/DMA у writer, відсутність нового
  software pacing і збережений partial-write/error handling;
- banner, alignment log, internal DRAM capacity/alignment та відсутність
  StreamBuffer symbols у новому ELF.

`verify_ab.py` охоплює всі вісім environments і selector 1..8, зберігає
попередні `verify_clocked`/`verify_spsc` checks і SHA-256 усіх 26 root-файлів.
Header відхиляє selector 9, відсутній selector та неузгоджені CMake/C selectors.
Метод виділення функції у `verify_clocked.py` лише розширено на `esp_err_t`
поряд із `void`/`size_t`; попередні критерії не послаблені.

`verify_spsc_runtime.py` за замовчуванням окремо компілює й запускає production
ring із selectors 7 та 8. Поряд із попередніми boundary/FIFO/in-flight tests
додані читання output-sized blocks: 256 циклів full-ring read/refill,
partial reads, alignment, unsigned rollover, повний/частковий overflow
із 4096-byte producer, finite-prefix flush із новим PCM за cutoff.
`gcd(32768,3840)=256`, отже 3840-byte reads відвідують 128 різних offsets;
assertions перевіряють двочастинні wrap copies. Тривалий тест має
**8000000 frames для кожного variant** з consumer buffer 4096 або 3840;
producer лишається до 4096. Також виконуються **100000 FIFO oracle steps
для кожного variant**. Усі assertions активні, NDEBUG відхиляється.

Host-тести виконують реальний C-код кільця з Windows threads; stubs замінюють
лише IDF placement/error/timer. Вони не перевіряють Bluetooth, ESP32 interrupts,
I²S clocks або гіпотезу на фізичній платі. Компілятор Zig 0.14.1 із попереднього
раунду залишається лише в ignored `.pio/host-tools`, firmware dependency немає.

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" tools/verify_ab.py
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" tools/verify_spsc.py
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" tools/verify_aligned.py
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" tools/verify_spsc_runtime.py --zig .pio/host-tools/ziglang/zig.exe
git diff --check
```

Source/preprocessed equality, semantic checks та strict ELF comparison
звітуються **окремо**. Старі hash failures не приховуються; CLI має exit 1,
якщо будь-яка його strict ELF перевірка FAIL, навіть при semantic PASS.
Це не автоматичне твердження про зміну поведінки. Не змінюємо SDK або linker
flags і не повторюємо збірки для підбору бажаного hash. Поточні hashes,
build sizes, host results і порівняння — `docs/verification.json`.

### Фізичний тест: NOT TESTED

Покрокова інструкція: [README: DMA-aligned у VS Code](../README.md#ручний-dma-aligned-тест-у-vs-code).
Окреме вікно control-проєкту → PlatformIO Project Tasks →
`control-spsc-dma-aligned` → General Build → Upload → Platform Monitor
115200 → EN/RESET. Перевірити banner і output/descriptor=3840,
writer_dma_aligned=yes, prefill=8192. Телефон → `Faital A2DP Control`,
Wi-Fi/hotspot off, той самий локальний трек, 20–30 см. Лише ESP32 + USB,
без DAC/підсилювача. Відтворювати 60–90 s; зберегти UART від RESET,
SBC/MTU/audio states, всі BT warnings і ≥12 груп `stats`/`SPSC`/`i2s`/`clocked`.

PASS: PCM близько 176400 B/s, бажано ≥99% (збережений коридор ±1%:
174636–178164), без регулярних sequence errors або gaps >100 ms; SPSC drops
після старту = 0; silence після prefill близька до нуля; I²S ~176400 B/s,
errors/shorts=0, ~229–230 writes/5 s. Start/pause/reconnect окремо від steady state.

Після тесту зіставити timestamps sequence errors із reference 0.34–0.35 s.
Зникнення errors підтримає гіпотезу; зміна періоду буде додатковим свідченням
залежності від writer/DMA phase, але сама по собі не є PASS. Якщо лишаться
ті ж ~0.35 s, кореляція могла бути випадковою; наступним **окремим** A/B
може бути priority. У цьому раунді priority не змінено, гіпотеза не підтверджена,
Upload/Monitor агент не запускає, root application не змінює.

### Фактичний результат локальних перевірок восьмого тесту

| Environment | Build | Static RAM bytes | App flash bytes | Source/config/isolation |
|---|---|---|---|---|
| control-baseline | SUCCESS | 54244 | 721949 | PASS |
| control-main-config | SUCCESS | 54244 | 721157 | PASS |
| control-streambuffer | SUCCESS | 54596 | 728077 | PASS |
| control-i2s-silence | SUCCESS | 54848 | 751069 | PASS |
| control-pipeline | SUCCESS | 54912 | 757509 | PASS |
| control-clocked-pipeline | SUCCESS | 55088 | 759849 | PASS |
| control-spsc-pipeline | SUCCESS | 87904 | 790133 | PASS |
| control-spsc-dma-aligned | SUCCESS | 87904 | 790325 | PASS |

Після основних builds виконано фінальну інкрементальну збірку всіх восьми
environments із остаточними source-коментарями; усі SUCCESS. Logs:
`.pio/aligned-build.log`, `.pio/aligned-legacy-build.log`,
`.pio/aligned-final-build.log` (ignored, локальні). Зменшення локального chunk
на 256 bytes не зменшує зарезервований task stack 8192 і static RAM; додатковий
boot log входить до flash. Фізичний результат aligned залишається NOT TESTED.

**Source/preprocessed equality — PASS:** 20/20 application translation units
попередніх семи variants і всі сім sdkconfig semantics незмінні. Основні
26 файлів відповідають SHA-256 protected manifest. `src/main.c`, I²S/StreamBuffer
modules і control headers, які не потребували зміни, також побайтово незмінні.

**Semantic verification — PASS:** усі вісім config/banner/source-list/isolation
checks, selector positive/negative checks, `verify_clocked`, SPSC semantics,
aligned compile-time contracts, повна нормалізована рівність transport та
рівність loadable SPSC/I²S object code/data між variants 7/8. `git diff --check`
і whitespace-перевірка нових/змінених файлів пройшли.

**Host tests — PASS:** обидва selectors, 4096/3840-byte consumer buffers,
по 100000 FIFO oracle steps і 8000000 ordered frames, спеціальні
output-sized wrap/partial/alignment/rollover/overflow/flush tests, а також
попередній test producer між copy та publication. Новий compiler/package
не встановлювався; використано вже наявний Zig у `.pio/host-tools`.

**Strict ELF comparison — FAIL**, критерії та очікувані хеші не змінені:

| Snapshot / verifier | PASS | FAIL |
|---|---|---|
| До variant 8, `verify_aligned.py` | baseline, main-config, streambuffer, i2s-silence | pipeline, clocked-pipeline, spsc-pipeline |
| До variant 7, `verify_spsc.py` | baseline, main-config, streambuffer, clocked-pipeline | i2s-silence, pipeline |

Різні snapshots пояснюють різні переліки: поточний clocked збігається зі
старішим snapshot до SPSC, але не з ELF на початку aligned-раунду. В усіх
трьох FAIL проти початку цього раунду збережені linker maps показують зміну
порядку LOAD бібліотек (`xtensa`, `bt`, `__pio_env`, `esp_driver_spi`,
`esp_driver_i2s`). У pipeline/clocked змінилися чотири allocated sections;
у SPSC — шість, включно з vectors/tdata. Це не лише timestamp.

Спостереження узгоджується з уже доведеною в попередньому раунді
невідтворюваністю clean link order без source/config edits. Воно не означає,
що доведено походження кожного відмінного байта або апаратну еквівалентність
images. ELF FAIL не приховано й не прирівнюється автоматично до зміни поведінки:
код/препроцесинг та семантика мають незалежні PASS. Повторних clean builds
для підбору потрібного hash не робили, SDK/linker flags не змінювали.

Exit codes: `verify_ab.py` **1**, `verify_spsc.py` **1**, `verify_aligned.py` **1**
через strict ELF; `verify_clocked.check_clocked` **0**, `verify_spsc_runtime.py`
**0**, `git diff --check` **0**. `docs/verification.json` містить усі expected /
actual hashes, окремі source/semantic/strict statuses, linker LOAD evidence,
host results, hardware NOT TESTED і точний diff конфігурації.

Змінені файли цього раунду: `platformio.ini`, `CMakeLists.txt`,
`src/CMakeLists.txt`, `src/control_variant.h`, `src/control_audio_config.h`,
`src/control_spsc.c`, `src/control_transport.c`, `tools/verify_ab.py`,
`tools/verify_clocked.py`, `tools/verify_spsc_runtime.py`, `tools/test_spsc.c`,
новий `tools/verify_aligned.py`, `README.md`, `docs/ab-investigation.md`,
`docs/verification.json`, новий `docs/aligned-reference.json`.

## 9. Official I²S reference

Дата реалізації: 2026-09-16. Environment `control-official-i2s-reference`,
selector **9**, hardware **NOT TESTED**. Upload/Monitor агент не запускав.
Це architecture reference із кількома відмінностями, а не однофакторний
експеримент `4096 → 3840`. Попередні фізичні результати вище збережені;
нових UART-даних для aligned або official reference не надано.

### Нормативне походження

Використано встановлений **ESP-IDF 6.1.0**, PlatformIO package
`platformio/framework-espidf@4.60100.0`, platform `espressif32@7.1.3`:

```text
C:/Users/1dima/.platformio/packages/framework-espidf/
  examples/bluetooth/bluedroid/classic_bt/a2dp_sink_stream/main/main.c
  examples/bluetooth/bluedroid/classic_bt/common/a2dp_utils/a2dp_sink_int_codec_utils/
    audio_sink_service_i2s.c
    audio_sink_service.h
    a2dp_sink_int_codec_utils.c
  components/esp_driver_i2s/include/driver/i2s_common.h
  components/esp_driver_i2s/i2s_common.c
  components/bt/host/bluedroid/btc/profile/std/a2dp/btc_a2dp_sink.c
```

`audio_sink_service_i2s.c` є джерелом `audio_sink_srv_data_output`, writer,
I²S initializer та SBC clock/slot mapping. `audio_sink_service.h` задає water
levels і три стани. `a2dp_sink_int_codec_utils.c` задає connection lifecycle.
`i2s_common.h` справді визначає **6/240/0**; `i2s_common.c` перевірено щодо
блокування write і disable. Ліцензійні заголовки Espressif збережено.
SHA-256 локальних джерел і snapshot **24 translation units** попередніх восьми
environments записані до редагування в `docs/official-reference.json`.

У `btc_a2dp_sink_handle_inc_media` sequence check виконується перед
`OI_CODEC_SBC_DecodeFrame` і `btc_a2d_data_cb_to_app`. `Pkt dropped` логується
при переповненні encoded SBC queue. Це не перевірка PCM format, але саме
повідомлення не локалізує місце втрати між source/RF/controller/host queues.
Жодних висновків про DMA-причинність без фізичного UART-тесту.

### Точна таблиця відмінностей

| Параметр | `control-spsc-dma-aligned` | `control-official-i2s-reference` | Офіційний local source |
|---|---|---|---|
| Selector / entry point | 8 / `main.c` | 9 / `control_official_main.c` | Example main + helper dispatcher |
| Service | Custom transport + SPSC | Окремий `control_official_i2s` | `audio_sink_service_i2s.c` |
| Buffer | Статичний SPSC 32768 bytes | BYTEBUF 32768 bytes | `xRingbufferCreate(32*1024, RINGBUF_TYPE_BYTEBUF)` |
| PCM send | Можливе часткове приймання | Цілий packet або drop, timeout 0 | `xRingbufferSend(..., 0)` |
| Prefetch | 8192, лише початковий | 20480, початковий і після underflow | 20*1024, PREFETCHING |
| Overflow | Приймається доступний frame-aligned prefix, хвіст drop | DROPPING; при fill ≤20480 → PROCESSING; цей packet теж drop | Те саме |
| Read | До 3840 bytes, timeout 0 | До 1440 bytes, timeout 20 ms | `240*6` саме bytes, не frames |
| Staging / padding / trim | 3840 + carry; padding є, latency trim немає; transition flush є | RingBuffer item, без staging/padding/trim | RingBuffer item, без staging/padding/trim |
| DMA desc × frames | 3×960 | 6×240, без override | `I2S_CHANNEL_DEFAULT_CONFIG` |
| Stereo descriptor / depth | 3840 / 11520 bytes | 960 / 5760 bytes | 240×4 / 6×240×4 |
| Depth @44.1 kHz | ~65.3 ms | ~32.65 ms | Те саме для stereo s16 |
| Interrupt priority / auto_clear | 0 / true | 0 / true | Macro default / `chan_cfg.auto_clear=true` |
| Slot format | Philips/I²S | MSB, s16, stereo default | `I2S_STD_MSB_SLOT_DEFAULT_CONFIG` |
| GPIO BCK/WS/DATA | 26/25/22 | 26/25/22 | Example Kconfig parameters |
| MCLK | Unused | Unused | Unused |
| Writer create / core | Pinned / core 1 | `xTaskCreate` / unpinned | `xTaskCreate` / unpinned |
| Priority / stack | 22 / 8192 | `configMAX_PRIORITIES-3` = 22 / 4096 | Те саме |
| Write timeout | 1000 ms | `portMAX_DELAY` argument | Те саме; API argument — milliseconds |
| Short write | Retry залишку | Лише counters, без retry | Один write, return code не перевіряється |
| Sample rate | Стартова 44100; зміни SBC застосовує writer | SBC: 16000/32000/44100/48000 | Та сама SBC flag precedence |
| Channels | Stereo | SBC mono/stereo, основний тест stereo | Такий самий mapping |
| Lifecycle | Постійний writer | Connection hooks + cooperative join | Connection hooks + immediate task delete |
| CPU / modem sleep | 160 MHz / on | **160 MHz / on**, підтверджено користувачем | Не задаються output-service C-кодом |
| PCM diagnostics | Timing і protected 64-bit stats | 32-bit relaxed counters, без timer/log/wait | Helper log кожні 100 packets, hot-path service logs |
| Stats task | 5-second custom reports | Priority 1, 5 s, `OFFICIAL stats` | Окремої такої task немає |

Повний generated sdkconfig нового environment порівнюється з aligned.
Окремий `control-main-config` має 240 MHz / sleep off і не є конфігурацією pipeline.
BLE release, Classic controller/host init, internal SBC, pairing, device name,
flash 4 MB, відсутність Wi-Fi initialization збережені. AVRCP-операцій не додано.
Основний PCM5102A тракт лишається Philips/I²S і не редагувався.

### State machine і hot path

Producer повторює upstream send/state logic. У DROPPING перевіряється fill;
при ≤20480 mode стає PROCESSING, **але поточний packet усе одно drop**.
PREFETCHING після накопичення ≥20480 дає binary semaphore. Writer читає до
1440 bytes з timeout 20 ms, робить один blocking write і повертає item.
Порожнє читання встановлює PREFETCHING і виходить із внутрішнього циклу.
Повторний запуск чекає нових 20480 bytes. Немає `xRingbufferReset`, pacing delay,
output buffer 4096/3840, retry, padding чи trim. State machine не оптимізували
і не виправляли навіть для теоретичних upstream races.

Callback не має власних critical sections/spinlocks, allocation, logs,
`esp_timer_get_time`, heap/stack diagnostics або очікування. Передається весь
packet. **FreeRTOS RingBuffer/semaphore APIs мають внутрішню синхронізацію**;
повністю lock-free callback не заявляється. Counters — 32-bit relaxed load/store
з одним власником; stats їх не скидає. Overflow entries збільшує producer в місці
переходу. Немає 64-bit atomics або RMW retry loops.

Окремі 32-bit lifetime flags захищають ring від видалення під callback/stats.
Reader встановлює active перед перевіркою running; stop спочатку закриває
running і чекає active=0. Цей протокол має seq_cst load/store; callback не чекає.
Xtensa assembly перевіряється на inline atomics без helpers і `s32c1i`.

### Intentional deviations від upstream

1. Збережено control Bluetooth init/name/GAP flow. Окремий entry point повторює
   їх без нового official app-work dispatcher або AVRCP дій. Старий `main.c`
   byte-identical; verifier порівнює Bluetooth init та GAP callback.
2. GPIO 26/25/22; замість example output Kconfig — ізоляція selector 9.
   SDK DMA defaults і MSB initializer збережено.
3. Hot-path packet/underflow/drop logs замінено counters та stats priority 1
   раз на 5 s. BT_APPL/BT_HCI warnings і log levels не змінено.
4. Додано error/short counters після одного write, без retry залишку.
5. Teardown кооперативний: закрити producer/stats доступ, дочекатися write
   і повернення item, delete task, disable channel, delete ring/semaphores.
   Close також delete channel. Додано completion semaphore. Upstream видаляє
   task без join; наш порядок не звільняє ресурси під активним writer.
   `vTaskDelay(1)` є лише в **stop lifecycle**, не у callback/writer.
   Partial initialization failure звільняє вже створені ресурси.
6. Open idempotent, із fallback для CONNECTED/CFG без CONNECTING. Initial CFG
   готує clock до CONNECTED. Live CFG виконує stop/reconfigure/start із новим
   prefetch; upstream helper лише stop/reconfigure і чекає наступного start.
   Буфер старого формату на цьому transition відкидається разом із ring.
7. AAC branch не перенесено: control використовує внутрішній SBC. SBC mapping
   включно mono і 16/32 kHz збережено; фізичний тест — stereo 44.1/48 kHz.

Після close немає старого writer, RingBuffer, prefetch/completion semaphore
чи I²S channel. Одна app-lifetime stats task залишається для idle/наступного
підключення; reconnect не створює її копій. Counters накопичуються від boot.
Відмінності вище потрібно враховувати при інтерпретації reference-тесту.

### Статистика

`cb`, `pcm_total`, `accepted`, `dropped_packets`, `dropped_bytes`, `underflows`,
`overflow_entries`, `i2s_written`, `writes`, `err`, `short` — totals від boot.
Stats будує 64-bit totals із modulo-32-bit deltas раз на 5 s; callback має лише
32-bit дані. `pcm_interval`, rates і `elapsed_ms` описують поточне вікно.
`ring` — миттєві доступні bytes за `vRingbufferGetInfo`, без reset/receive.
Snapshot counters не є транзакцією між ядрами. Mode: 0=PROCESSING,
1=PREFETCHING, 2=DROPPING. Idle/початкові/pause/reconnect windows треба відділяти
від сталого потоку. Після навмисної pause один underflow очікуваний.
Pending PCM при close не додається до dropped packets: його прийнято до disconnect.

`auto_clear=true` збережено: driver може обнулювати DMA без нових даних.
**Software padding немає**. I²S rate — bytes, прийняті write API, а не незалежно
виміряні фізичні clocks чи auto-cleared DMA data. Звук/MSB не оцінюється.

### Перевірки та межі

`verify_official.py` порівнює **24** старі preprocessed application units,
вісім configs і незмінні sources зі snapshot. Перевіряє source list, ELF symbols,
banner, callback, відсутність custom transports і фактичні DMA/config values
через object-probe, скомпільований Xtensa. Старі root/config/selector checks збережені.

На нову вимогу користувача historical ELF identity є **advisory**.
`verify_spsc.py --strict-elf` і `verify_aligned.py --strict-elf` відтворюють
старий суворий критерій; expected hashes не змінено. Default scripts перевіряють
source/config/semantics, залишаючи `strict_elf_checks` і section differences
у JSON. Історичні FAIL/exit 1 у попередніх розділах не переписано.

`verify_official_runtime.py` компілює **production service C** з Windows threads
і test-only API adapters. Перевіряє prefetch/re-prefetch, wrap/order, whole-packet
overflow/drop-exit packet, errors/shorts без retry, 44.1/48 kHz і mono/stereo,
live codec restart, close під час writer/callback/stats access, сім точок partial
initialization failure та 32 reconnects без залишених service resources.
Це не перевіряє реальний FreeRTOS RingBuffer/scheduler, DMA чи Bluetooth.
Старі production SPSC host-тести також повторюються.

Остаточні build sizes, exit codes, source/semantic/ELF results і root hashes —
у `docs/verification.json`. Build/host PASS не є physical PASS.

### Ручний тест і дані для повернення

1. Окреме вікно VS Code: `C:\bluetooth-speaker\control\a2dp_sink_control`.
2. PlatformIO → Project Tasks.
3. `control-official-i2s-reference`.
4. General → Build.
5. General → Upload (старий Monitor закритий).
6. Platform → Monitor, **115200**.
7. EN/RESET.
8. Banner: `official-i2s-reference`, source ESP-IDF-6.1, ring 32768, prefetch 20480,
   DMA 6×240, write_upto 1440, priority 22/core unpinned, MSB/no audible validation.
   Підключити телефон до `Faital A2DP Control`.
9. Лише ESP32 від USB.
10. DAC і підсилювач не підключати.
11. Wi-Fi/hotspot телефона вимкнути.
12. Відстань 20–30 см.
13. Той самий завантажений локальний трек.
14. Безперервне відтворення **90 секунд**.
15. Повний UART від RESET: SBC/MTU/audio states, ≥12 послідовних `OFFICIAL stats`,
    усі BT_APPL/BT_HCI warnings. Окремо після тесту зберегти disconnect/reconnect
    і нове відтворення для перевірки lifecycle.

Додати модель телефона/плеєр, negotiated frequency/channels, умови й тривалість.
PASS @44.1 kHz stereo: PCM ~176400 B/s, після prefetch без регулярних underflows
і sequence errors, без `Pkt dropped`, dropped_packets=0, I²S err/short=0.
@48 kHz stereo expected=192000 B/s. FAIL: регулярні 141–151 kB/s @44.1 kHz,
sequence errors, underflows/drops.

PASS підтримає пошук у відмінностях custom transport/writer/scheduling, але
ця багатопараметрична заміна не визначає конкретну винну функцію. Аналогічний
FAIL послабить гіпотезу про унікальний SPSC/StreamBuffer дефект та спрямовуватиме
на спільну взаємодію I²S DMA/interrupt cadence з Bluetooth, IDF або плату.
Остаточного висновку без фізичного UART-логу немає.

### Підсумок реалізації reference

Усі дев'ять environments **Build SUCCESS**. Новий environment: RAM **54528 bytes**,
flash **759229 bytes**. `verify_ab`, `verify_clocked`, `verify_spsc`,
`verify_aligned`, `verify_official`, `verify_spsc_runtime` і
`verify_official_runtime` повернули **exit 0**. `git diff --check` — **PASS**.
Незмінність **24/24** preprocessed units, восьми повних sdkconfigs і **26/26**
root-файлів підтверджена. Остаточні build sizes усіх variants є в JSON.

У поточній збірці advisory ELF differences проти reference до SPSC:
streambuffer, i2s-silence, pipeline, clocked-pipeline; проти reference до aligned:
streambuffer, pipeline, spsc-pipeline. Це незалежні snapshots. Повні expected/
actual section hashes залишилися у звіті; вони не підміняють source/semantic PASS.
Причину кожного відмінного байта в цьому раунді окремо не доводили. Історичний
аудит зміни link order при незмінних sources збережено в historical report.

Змінено/додано 18 файлів: `platformio.ini`, `CMakeLists.txt`, `src/CMakeLists.txt`,
`src/control_variant.h`, `src/control_official_main.c`, `src/control_official_i2s.c`,
`src/control_official_i2s.h`, `tools/verify_ab.py`, `tools/verify_spsc.py`,
`tools/verify_aligned.py`, `tools/verify_official.py`, `tools/verify_official_runtime.py`,
`tools/test_official.c`, `tools/official_host_stubs.h`, `README.md`,
`docs/ab-investigation.md`, `docs/verification.json`, `docs/official-reference.json`.
Generated builds/test adapters/cache залишилися в ignored `.pio`.

## 10. CPU frequency × Bluetooth modem sleep

Поточний раунд: **2026-09-17–18**. Hardware test матриці — **NOT RUN**.
Upload, Monitor, commit і push агент не виконує. Зміни лише в control-сабпроєкті.
AGENTS.md у сабпроєкті та його батьківських директоріях не знайдено.

### Нові історичні апаратні дані

За новим повідомленням користувача, без сирих UART-файлів у цьому завданні:

| Історичний environment | PCM при expected 176400 B/s | Sequence errors | Інші спостереження | Статус |
|---|---:|---|---|---|
| control-spsc-dma-aligned | ~141200 B/s | ~кожні 350 ms | I²S err/short=0, короткий catch-up наприкінці | Historical FAIL |
| control-official-i2s-reference, 160 MHz / sleep ON | ~152000 B/s | ~кожні 290 ms | Underflows і application drops; I²S err/short=0; короткий catch-up | Historical FAIL |

Короткі завершальні вікна ~176400 B/s не є стабільним PASS. Раніші записи
NOT TESTED/NOT RUN вище описують стан попередніх раундів, а не ці нові повідомлення.
Фізичний повтор reference у поточній матриці ще не виконано.

### Матриця і незмінний алгоритм

| Environment | CPU | Controller modem sleep | Selector | Новий hardware run |
|---|---:|---|---:|---|
| control-official-i2s-reference | 160 MHz | ON | 9 | NOT RUN |
| control-official-160-nosleep | 160 MHz | OFF | 9 | NOT RUN |
| control-official-240-sleep | 240 MHz | ON | 9 | NOT RUN |
| control-official-240-nosleep | 240 MHz | OFF | 9 | NOT RUN |

Є одна реалізація: `control_official_main.c` + `control_official_i2s.c`.
Змінено лише startup diagnostics у `control_official_i2s_init` і потрібні для
нього declarations. `control_official_main.c` лишається byte-identical. Усі інші
функції service збережені, включно фактичними lifecycle/atomic flags, описаними
в розділі 9. Нових виправлень state machine або scheduling у матриці немає.

Незмінні: BYTEBUF 32768; prefetch/re-prefetch 20480; whole-packet admission;
PREFETCHING/PROCESSING/DROPPING; DMA 6×240; read up to 1440 bytes / timeout 20 ms;
priority configMAX_PRIORITIES-3 = 22; unpinned writer; stacks 4096; interrupt
allocation; auto_clear; MSB/s16; GPIO26/25/22, MCLK unused; SBC decoder,
Bluetooth init/callbacks/MTU; Faital A2DP Control; stats priority 1 / 5 seconds.
Platform espressif32 7.1.3, framework 4.60100.0 / IDF 6.1.0 і встановлений
Xtensa toolchain не замінювалися. Source/package hashes записано до змін.

### Нормативні локальні джерела Kconfig і builder

Префікс SDK: `C:/Users/1dima/.platformio/packages/framework-espidf/`.

- `components/esp_system/port/soc/esp32/Kconfig.cpu`: choice
  `ESP_DEFAULT_CPU_FREQ_MHZ_160` / `_240`; числовий `ESP_DEFAULT_CPU_FREQ_MHZ`
  є похідним. Частоту встановлює стандартний IDF startup; runtime switching немає.
- `components/esp_system/sdkconfig.rename.esp32`: відповідні
  `ESP32_DEFAULT_CPU_FREQ_*` — сумісні старі назви, а не додаткові фактори.
- `components/bt/controller/esp32/Kconfig.in`: `BTDM_CTRL_MODEM_SLEEP`
  вмикає controller low-power mode; ORIG залежить від sleep, LPCLK selection
  залежить від ORIG. ON повторює reference ORIG + MAIN_XTAL.
- `components/bt/sdkconfig.rename`: `BTDM_CONTROLLER_MODEM_SLEEP` є legacy alias.
- `components/bt/controller/esp32/bt.c`: initialization обирає ORIG при
  enabled ORIG config, інакше NONE (EVED не використовується); controller enable
  викликає `btdm_controller_enable_sleep(true)` лише для ORIG. У застосунку немає
  `esp_bt_sleep_enable/disable`, sniff-policy changes, runtime CPU switching,
  нових PM locks або light-sleep calls.
- `components/esp_hw_support/include/esp_clk_tree.h` і
  `port/esp32/esp_clk_tree.c`: public `esp_clk_tree_src_get_freq_hz` для
  `SOC_MOD_CLK_CPU` читає `clk_hal_cpu_get_freq_hz`; CPU case не виконує calibration,
  frequency switching чи lock acquisition.
- Builder: `C:/Users/1dima/.platformio/platforms/espressif32/builder/frameworks/espidf.py`.
  `SDKCONFIG_PATH` читає `build.esp-idf.sdkconfig_path`; типовий шлях —
  `sdkconfig.$PIOENV`. Builder передає `-DSDKCONFIG=...` та підтримує
  `build.cmake_extra_args`. Root CMake задає `SDKCONFIG_DEFAULTS` до project init.

Це controller **modem sleep**, не Bluetooth link sniff mode і не ESP32 light sleep.
`CONFIG_PM_ENABLE` лишається off. XTAL frequency і RTC clock source не змінюються.
В OFF selection BT low-power clock стає неактивною залежністю; інший clock source
не обирається.

### Фактичні semantic config differences

Відносно 160 MHz / sleep ON дозволено лише такі 10 keys; disabled/absent
нормалізуються як 0, aliases у `sdkconfig.h` розгортаються, hex/decimal порівнюються
за значенням. Legacy keys не прописані вручну в defaults.

| Kconfig symbol | Reference | 160/OFF | 240/ON | 240/OFF | Зв'язок |
|---|---|---|---|---|---|
| CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160 | y | y | n | n | CPU choice sibling |
| CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240 | n | n | y | y | CPU choice |
| CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ | 160 | 160 | 240 | 240 | Похідне число |
| CONFIG_ESP32_DEFAULT_CPU_FREQ_160 | y | y | n | n | Legacy alias |
| CONFIG_ESP32_DEFAULT_CPU_FREQ_240 | n | n | y | y | Legacy alias |
| CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ | 160 | 160 | 240 | 240 | Legacy alias |
| CONFIG_BTDM_CTRL_MODEM_SLEEP | y | n | y | n | Controller modem sleep |
| CONFIG_BTDM_CTRL_MODEM_SLEEP_MODE_ORIG | y | n/absent | y | n/absent | Залежить від sleep |
| CONFIG_BTDM_CTRL_LPCLK_SEL_MAIN_XTAL | y | n/absent | y | n/absent | Залежить від ORIG |
| CONFIG_BTDM_CONTROLLER_MODEM_SLEEP | y | n | y | n | Legacy alias |

Отже, 160/OFF має **4**, 240/ON — **6**, 240/OFF — **10** відмінних keys.
Будь-яка інша semantic відмінність — blocker чистого порівняння, її не маскують.

### Ізоляція generated configuration

Чотири explicit `board_build.esp-idf.sdkconfig_path` вказують на чотири окремі
`sdkconfig.<environment>`. Build directories: `.pio/build/<environment>`.
Defaults overlays: `sdkconfig.official-160-sleep.defaults`,
`sdkconfig.official-160-nosleep.defaults`, `sdkconfig.official-240-sleep.defaults`,
`sdkconfig.official-240-nosleep.defaults`, кожен після незмінного `sdkconfig.defaults`.
Кожен overlay задає лише CPU choice і modem sleep.

Generated sdkconfig має пріоритет над defaults. Тому `matrix_config_guard.py`
працює як pre і post extra script навіть при incremental Build. Він перевіряє
унікальність config/build paths, правильні CPU/sleep та повну semantic рівність
решти config із snapshot reference. Перевіряється також `config/sdkconfig.h`.
Guard лише читає; stale config зупиняє Build, не переписується й не видаляється.
Стандартні Clean/Menuconfig залишаються доступними для локального виправлення.
Наявні generated configs користувача масово не видалялися/перезаписувалися.

`docs/matrix-reference.json` містить snapshot до редагування: 26 preprocessed
units дев'яти environments, hashes існуючих configs/sources, baseline settings,
hashes усіх non-startup official functions і локальних Kconfig/controller/builder.
Старі reference JSON не перегенеровуються для приховування FAIL.

### Startup identification і верифікація

Для кожного environment додано:

```text
CONTROL env=<environment> variant=official-i2s-reference
CONTROL cpu_config_mhz=<160|240> bt_modem_sleep_config=<enabled|disabled>
CONTROL cpu_runtime_hz=<Hz> clock_read_result=<esp_err_t> source=esp_clk_tree_src_get_freq_hz
```

CPU/sleep banner читає generated CONFIG macros; environment-name define відповідає
лише за ім'я. Disabled modem sleep коректно обробляється через `#if`, коли macro
відсутній у header. CPU API викликається один раз до Bluetooth startup; фактичний
runtime результат стане відомим з UART. Modem-sleep setting не оголошується
виміряним runtime state. DMA/ring/prefetch/writer/MSB banner збережено.

`verify_matrix.py` перевіряє config/header parity, лише дозволений diff,
same sources і preprocessed non-startup functions у чотирьох builds, immutable
source bodies від reference, ELF banners/symbol isolation, Xtensa compile-time
contract probes і відсутність runtime overrides. Старі 24 translation units
восьми variants та всі дев'ять старих sdkconfigs порівнюються зі snapshot.
ELF identity між CPU/sleep profiles не вимагається.

Build audit виконує reference → 160/OFF → 240/ON → 240/OFF → reference повторно.
До/після кожної збірки перевіряються hashes усіх generated config files:
дозволені зміни лише target environment, а repeated reference має ті самі settings.
Цей порядок є software verification, фізичний порядок наведено нижче.
`test_matrix_guard.py` перевіряє 28 isolated fixtures: valid/stale CPU/sleep/header,
unrelated PM change і shared config/build paths. Guard не змінює fixture bytes.

### Результат локальної перевірки, 2026-09-18

| Перевірка | Статус | Підтвердження |
|---|---|---|
| Build | **PASS** | Чотири environments, потім повтор reference; також усі вісім попередніх variants |
| Config-isolation | **PASS** | Окремі paths; sdkconfig/header parity; лише 4/6/10 дозволених відмінностей; немає змін конфігів інших environments |
| Verification | **PASS** | `verify_ab` із clocked/SPSC/aligned/official/matrix checks, guard 28/28, official і два SPSC host-тести, whitespace/diff checks |
| Hardware test | **NOT RUN** | Upload/Monitor не запускалися; commit/push не виконувалися |

Усі чотири прошивки: RAM **54528 bytes**. Flash code/data: reference **759545**,
160/OFF **758273**, 240/ON **759561**, 240/OFF **758709 bytes**.
У чотирьох ELF підтверджено унікальні env banners, потрібний official service
і відсутність custom transports. Скомпільовані non-startup function bodies однакові.

**24/24** старих preprocessed translation units та семантика всіх **9** існуючих
sdkconfigs збережені; **26/26** захищених файлів основної прошивки незмінні.
Вісім неофіційних sdkconfig лишилися byte-identical. У generated reference
Kconfig прибрав лише чотири коментарі `# default:` після явного CPU/sleep overlay;
усі значення та решта bytes збережені. Під час зафіксованої послідовності п'яти
matrix builds конфіги інших environments не змінювалися.

Початкову збірку 240/OFF і host-test executable блокувала Windows Application
Control (4551). Повтор тієї самої збірки та host-тест у відновленій сесії пройшли
без змін security policy, SDK/toolchain чи коду для обходу блокування. Перервану
сесію регресійної збірки продовжено; фінально всі Build і verification — exit 0.

Повний [машинний звіт](verification.json) містить config diff, список **22**
змінених/доданих файлів цього раунду, build audit та попередній звіт окремо як
`historical_official_round`. Історичні ELF comparisons лишаються advisory **FAIL**;
expected hashes не оновлювалися. Host-тести не вимірюють фізичний вплив CPU/sleep.

### Фізичний протокол і майбутні результати

Порядок: **160/OFF → 240/ON → 240/OFF → reference 160/ON**.
Останній запуск перевіряє незмінність зовнішніх умов, а не успадковує historical FAIL.

У VS Code окремим вікном відкрити `C:\bluetooth-speaker\control\a2dp_sink_control`.
**PlatformIO → Project Tasks → потрібний environment → General → Build → Upload →
Platform → Monitor (115200) → EN/RESET**. Перевірити env/CPU/sleep/DMA banner,
підключити телефон до Faital A2DP Control. Закривати Monitor перед наступним Upload.

Та сама ESP32, USB, телефон, розташування; без DAC/підсилювача; Wi-Fi/hotspot off.
Той самий downloaded track із тієї самої позиції. 90 секунд без pause/seek/зміни
гучності, потім pause. Логи окремо: `<environment>.log`, повністю від RESET,
із SBC/MTU/audio states, ≥12 повними stats intervals і всіма warnings.

| Environment | CPU runtime / negotiated format | PCM / expected | Sequence count / period | Drops / underflows | I²S err / short | Результат і повтор |
|---|---|---|---|---|---|---|
| control-official-160-nosleep | — | — | — | — | — | NOT RUN |
| control-official-240-sleep | — | — | — | — | — | NOT RUN |
| control-official-240-nosleep | — | — | — | — | — | NOT RUN |
| control-official-i2s-reference — повтор | — | — | — | — | — | NOT RUN |

Expected = negotiated sample rate × channels × 2: 176400 B/s @44.1 kHz stereo s16,
192000 B/s @48 kHz stereo. Оцінювати кілька повних windows після prefetch;
накопичувальні drops/underflows порівнювати за приростами. PASS: PCM близький до
expected, без регулярних sequence errors/application drops/underflows,
I²S err/short=0. Один добрий рядок, startup average або catch-up у кінці — не PASS.
**Будь-який PASS повторити**, перш ніж вважати результат відтворюваним.

160/OFF проти reference ізолює sleep; 240/ON проти reference — CPU; порівняння
240/OFF із двома проміжними точками допомагає оцінити взаємодію факторів.
Різниця вкаже на їхній вплив, але не доведе конкретний механізм дефекту.
Повернути чотири повні UART-логи, модель телефона/плеєр, format, умови й повтор PASS.
