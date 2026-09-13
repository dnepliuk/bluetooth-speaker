# Дослідження втрат Bluetooth / HCI

За наданим тестом чистий локальний tone понад 30 секунд дає 176399–176400 B/s без I²S errors, short writes та underflow. У Bluetooth-тесті `i2s_rate` приблизно дорівнює заниженому `rx_rate`, а `dropped`, `invalid_pcm` і `latency_trim` дорівнюють нулю. Це локалізує дефіцит даних **до входу decoded PCM у наш StreamBuffer**. Повторний HCI warning підтверджує відкидання незавершених L2CAP PDU у host reassembly. Радіоканал, source, controller/VHCI та дефіцит ресурсів залишаються гіпотезами; конкретний винуватець без нового тесту на платі не доведений.

Це результати користувача, а не новий фізичний тест, виконаний агентом. Збірка перевіряє сумісність коду, але не підтверджує усунення переривань.

## Що означає HCI warning

У встановленому ESP-IDF 6.1: [packet_fragmenter.c:143](C:/Users/1dima/.platformio/packages/framework-espidf/components/bt/host/bluedroid/hci/packet_fragmenter.c:143), `reassemble_and_dispatch()`; потрібний warning — **рядок 172**.

Функція виділяє 12-бітний connection handle і packet-boundary flag із HCI ACL header. Для початкового фрагмента читає L2CAP length і очікує загалом `length + 4` байти L2CAP header та `4` байти HCI header. Незавершену збірку зберігає в `partial_packets[handle]`; continuation додає до її поточного offset. У верхній рівень пакет передається після досягнення очікуваної довжини.

`found unfinished packet ... with start packet. Dropping old.` виникає, коли для **того самого handle** вже є незавершена збірка, але прийшов новий START. Стару збірку видаляють і звільняють. У цій функції немає таймера очікування, який сам генерує warning. Це не повідомлення про переповнення нашого PCM buffer. Сам рядок не містить значення handle/CID, тому не дозволяє приписати кожну подію саме media channel або перетворити кількість warning на кількість втрачених SBC frames / PCM bytes.

Можливі причини стану: відсутній continuation, скидання/втрата фрагмента раніше, неправильні boundary/length, flush або помилка source/controller/host. Warning сам не вимірює over-the-air packet loss. Збіг із underflow підтримує зв'язок із дефіцитом аудіо, але ще не доводить конкретний механізм.

```mermaid
flowchart LR
    A[Source / radio / controller] --> B[VHCI RX FIFO: 254]
    B --> C[HCI ACL → L2CAP reassembly]
    C --> D[BTU / L2CAP / AVDTP]
    D --> E[SBC queue: 25]
    E --> F[BTC decoder / PCM callback]
    F --> G[StreamBuffer: 32 KiB]
    G --> H[I²S writer / DMA]
```

Важливі межі черг і flow control:

| Місце | Що фактично робить встановлений SDK |
| --- | --- |
| [hci_hal_h4.c:136](C:/Users/1dima/.platformio/packages/framework-espidf/components/bt/host/bluedroid/hci/hci_hal_h4.c:136), [fixed_queue.h:27](C:/Users/1dima/.platformio/packages/framework-espidf/components/bt/common/osi/include/osi/fixed_queue.h:27) | RX FIFO має `QUEUE_SIZE_MAX=254`; це окрема черга ACL/events, а не одноелементна work queue. |
| [host_recv_pkt_cb:607](C:/Users/1dima/.platformio/packages/framework-espidf/components/bt/host/bluedroid/hci/hci_hal_h4.c:607) | Виділяє `BT_HDR_SIZE + len`, копіює H4-пакет, викликає `fixed_queue_enqueue(..., FIXED_QUEUE_MAX_TIMEOUT)`. Повна FIFO чекає; виділення packet buffer при невдачі дає error/assert. |
| [fixed_queue_enqueue:137](C:/Users/1dima/.platformio/packages/framework-espidf/components/bt/common/osi/fixed_queue.c:137), [list_append:152](C:/Users/1dima/.platformio/packages/framework-espidf/components/bt/common/osi/list.c:152) | Вузол списку також потребує heap. Помилка allocation дає OSI error і `false`; `host_recv_pkt_cb` не перевіряє повернений результат enqueue. Це реальний можливий шлях втрати до reassembly, але наданий лог не доводить allocation failure. Framework не патчився. |
| [hci_upstream_data_handler:247](C:/Users/1dima/.platformio/packages/framework-espidf/components/bt/host/bluedroid/hci/hci_hal_h4.c:247), [hal_says_packet_ready:429](C:/Users/1dima/.platformio/packages/framework-espidf/components/bt/host/bluedroid/hci/hci_layer.c:429) | HCI task дренує FIFO, перевіряє H4 і передає ACL до fragmenter. Дві work queues HCI довжиною 1 — сповіщення роботи, а не місткість ACL FIFO. |
| [dispatch_reassembled:570](C:/Users/1dima/.platformio/packages/framework-espidf/components/bt/host/bluedroid/hci/hci_layer.c:570), [btu_task_post:219](C:/Users/1dima/.platformio/packages/framework-espidf/components/bt/host/bluedroid/stack/btu/btu_task.c:219) | Готовий пакет передається BTU з очікуванням. BTU work queue використовує default 100; невдалий post звільняє вже зібраний пакет. Це інша стадія втрат. Default визначений у [osi/thread.c:62](C:/Users/1dima/.platformio/packages/framework-espidf/components/bt/common/osi/thread.c:62). |
| [btc_a2dp_sink_enque_buf:677](C:/Users/1dima/.platformio/packages/framework-espidf/components/bt/host/bluedroid/btc/profile/std/a2dp/btc_a2dp_sink.c:677) | `RxSbcQ` обмежена перевіркою 25 пакетів; переповнення дає `Pkt dropped`. Черга розташована **після** reassembly. Декодування й PCM callback виконуються на BTC task (`btc_get_current_thread`, рядок 209), не на нашому writer. |
| [controller.c:121](C:/Users/1dima/.platformio/packages/framework-espidf/components/bt/host/bluedroid/device/controller.c:121), [bt_target.h:1498](C:/Users/1dima/.platformio/packages/framework-espidf/components/bt/host/bluedroid/common/include/common/bt_target.h:1498) | Controller→host ACL flow control увімкнений. Host рекламує `L2CAP_MTU_SIZE=1691` і `L2CAP_HOST_FC_ACL_BUFS=20` (рядки 1575, 1585). Runtime Read Buffer Size повертає місткість controller для іншого напрямку, host→controller; не плутати її з 20 host credits. |
| [hci_packet_complete:290](C:/Users/1dima/.platformio/packages/framework-espidf/components/bt/host/bluedroid/hci/hci_hal_h4.c:290) | Host повертає credit за ACL-фрагмент перед подальшим dispatch (виклик у рядку 464). Credit не означає, що SBC decoder уже обробив PDU. |

`BT_DEFAULT_BUFFER_SIZE=4096+16` використовується як default кількох протокольних буферів, але не задає фіксований розмір кожного VHCI RX allocation. `CONFIG_BT_ACL_CONNECTIONS=4` і `CONFIG_BTDM_CTRL_BR_EDR_MAX_ACL_CONN=2` задають **кількість з'єднань**, не кількість ACL-пакетів. Відповідні Kconfig: [host:1329](C:/Users/1dima/.platformio/packages/framework-espidf/components/bt/host/bluedroid/Kconfig.in:1329), [controller:34](C:/Users/1dima/.platformio/packages/framework-espidf/components/bt/controller/esp32/Kconfig.in:34). Їх збільшення без вимірювання не є обґрунтованим fix.

## Наші callbacks, задачі та effective config

[a2dp_data_callback](C:/bluetooth-speaker/src/bluetooth_audio.c:199) викликає [audio_pipeline_receive_pcm](C:/bluetooth-speaker/src/audio_pipeline.c:707). Тут один producer, копіювання цілих stereo frames через `xStreamBufferSend(..., 0)`, коротка critical section із лічильниками. Немає I²S, UART logging, heap allocation або очікування вільного місця. Новий `callback_copy_lock_max_us` вимірює копіювання та очікування входу в stats lock; він не є профілем усього decoder.

| Задача | Priority / core | Stack bytes |
| --- | --- | --- |
| Controller | 23 / 0 | 4096 |
| HCI host | 22 / 0 | 2560 |
| BTU | 20 / 0 | 4864 |
| BTC, включно з SBC / PCM callback | 19 / 0 | 3584 |
| Наш writer | 22 / 1 | 8192 |
| Наші reconnect / storage / LED / stats / RSSI | 4 / 3 / 2 / 1 / 1; без affinity | 3072 / 3072 / 3072 / 4096 / 3072 |
| Тимчасовий I²S init | 22 / 1 | 4096 |

Джерела SDK: [esp_task.h:32](C:/Users/1dima/.platformio/packages/framework-espidf/components/esp_system/include/esp_task.h:32), [hci_layer.c:45](C:/Users/1dima/.platformio/packages/framework-espidf/components/bt/host/bluedroid/hci/hci_layer.c:45), [btu_init.c:48](C:/Users/1dima/.platformio/packages/framework-espidf/components/bt/host/bluedroid/stack/btu/btu_init.c:48), [btc_task.c:97](C:/Users/1dima/.platformio/packages/framework-espidf/components/bt/common/btc/core/btc_task.c:97). У цій конфігурації `TASK_EXTRA_STACK_SIZE=512`, тому Kconfig значення BTC/BTU `3072/4352` нижчі за фактично виділені стеки `3584/4864`. Startup log явно називає їх `stack_config`.

Наші фонові задачі не мають пріоритету вище BT на core 0. Це знижує ймовірність прямого priority starvation, але не виключає flash/cache stalls або внутрішніх блокувань SDK. Збереження MAC виконується в окремій задачі й пропускає запис тієї самої адреси; NVS commit може тимчасово зупиняти flash-dependent виконання при новому peer. RSSI — один запит раз на 5 секунд після початкових 2.5 секунди. Reconnect — одна спроба після 1.2 секунди. Event callbacks логують події підключення/налаштування, тому синхронний UART може затримувати BTC під час таких подій; PCM callback не логує. LED лише періодично оновлює GPIO.

Перевірено `sdkconfig.defaults` та обидва generated configs: Classic BR/EDR-only, Bluedroid, внутрішній SBC codec, BLE вимкнений, VHCI, controller/host core 0, CPU 240 MHz, FreeRTOS 100 Hz, два ядра, modem sleep вимкнений. HCI/APPL/A2D/AVDT compile trace — WARN; global DEBUG не вмикався. Stack canary і task watchdog увімкнені; heap poisoning та abort-on-allocation-failure вимкнені. Wi-Fi/coexistence support скомпільований, але в нашому коді немає `esp_wifi_init/start`; це не доказ активного Wi-Fi radio. Стековий VSC coexistence default сам не означає конкуренцію з працюючим Wi-Fi.

Relevant Kconfig: [host core/stacks:1](C:/Users/1dima/.platformio/packages/framework-espidf/components/bt/host/bluedroid/Kconfig.in:1), [internal codec:100](C:/Users/1dima/.platformio/packages/framework-espidf/components/bt/host/bluedroid/Kconfig.in:100), [HCI WARN:585](C:/Users/1dima/.platformio/packages/framework-espidf/components/bt/host/bluedroid/Kconfig.in:585), [APPL WARN:1083](C:/Users/1dima/.platformio/packages/framework-espidf/components/bt/host/bluedroid/Kconfig.in:1083), [VHCI/modem sleep:223](C:/Users/1dima/.platformio/packages/framework-espidf/components/bt/controller/esp32/Kconfig.in:223).

`0x04` — HCI Page Timeout; помилка початкового reconnect не доводить причину наступних media gaps. `0x24` / decimal 36 — LMP PDU Not Allowed, а не код buffer overflow; з нього не випливає, що локальний modem sleep увімкнений. Визначення: [hcidefs.h:1056](C:/Users/1dima/.platformio/packages/framework-espidf/components/bt/host/bluedroid/stack/include/stack/hcidefs.h:1056), [hcidefs.h:1088](C:/Users/1dima/.platformio/packages/framework-espidf/components/bt/host/bluedroid/stack/include/stack/hcidefs.h:1088). Нові GAP logs додають фактичні link mode / interval і ACL status / disconnect reason.

`rssi_delta=-16…-17 dB` не означає `-16…-17 dBm`. Нуль означає Golden Receive Power Range; від'ємне значення показує відхилення нижче нижньої межі, додатне — вище верхньої. Абсолютний RSSI з цього логу не обчислювався. Це підстава перевірити розташування/антену, але не локалізація втрати. Джерела: [installed GAP API:375](C:/Users/1dima/.platformio/packages/framework-espidf/components/bt/host/bluedroid/api/include/api/esp_gap_bt_api.h:375), Bluetooth Core, Vol 4 Part E §7.5.4 ([копія специфікації на Renesas](https://community.renesas.com/cfs-file/__key/communityserver-discussions-components-files/297/Core_5F00_v5.2.pdf)).

## Порівняння з офіційним a2dp_sink_stream

Reference — [installed main.c](C:/Users/1dima/.platformio/packages/framework-espidf/examples/bluetooth/bluedroid/classic_bt/a2dp_sink_stream/main/main.c:118) і helper components у `classic_bt/common`.

| Частина | Приклад / наш проєкт | Висновок |
| --- | --- | --- |
| Controller, Bluedroid | NVS, release BLE, default controller config, Classic enable; host default config / `esp_bluedroid_init()` | Еквівалентний init: наш wrapper теж використовує default config. [Helper:140](C:/Users/1dima/.platformio/packages/framework-espidf/examples/bluetooth/bluedroid/classic_bt/common/bredr_app_common_utils/bredr_app_common_utils.c:140), [API wrapper:112](C:/Users/1dima/.platformio/packages/framework-espidf/components/bt/host/bluedroid/api/esp_bt_main.c:112). |
| AVRCP / A2DP | Stream example не ініціалізує AVRCP; у нас CT перед A2DP | Такий порядок правильний, коли AVRCP використовується. Minimal mode дозволяє перевірити його вплив. |
| A2DP callbacks | register event callback → sink init → register PCM callback | У нас та сама послідовність; external-codec API з іншої гілки прикладу не потрібний. |
| Event dispatch | Example копіює event у BtAppTask, priority 10, stack 4096, queue 10, send timeout 10 ms; у нас handler на BTC | Відмінність не доводить дефект. Наш PCM шлях не має важкої event-обробки; рідкі event logs можуть затримувати BTC. |
| PCM handoff | Example `xRingbufferSend(...,0)`; у нас `xStreamBufferSend(...,0)` | Обидва неблокуючі producer paths. Example ще логує кожні 100 PCM callbacks; це не копіювалося. |
| Buffer / writer | Example 32 KiB, prefetch 20 KiB, writer priority 22 без affinity; у нас 32 KiB / 8 KiB, priority 22 core 1 | Великий prefetch може відкласти underflow, але не компенсує тривалий дефіцит `rx_rate`. [Service config](C:/Users/1dima/.platformio/packages/framework-espidf/examples/bluetooth/bluedroid/classic_bt/common/a2dp_utils/a2dp_sink_int_codec_utils/audio_sink_service.h:14). |
| I²S | Example MSB slot format; у нас перевірений Philips, s16 stereo | [Example I²S:98](C:/Users/1dima/.platformio/packages/framework-espidf/examples/bluetooth/bluedroid/classic_bt/common/a2dp_utils/a2dp_sink_int_codec_utils/audio_sink_service_i2s.c:98) не є підставою змінити PCM5102A format або GPIO. |
| BT memory defaults | Обидва використовують штатні host/controller settings та internal decoder | У reference не знайдено спеціального ACL buffer fix, якого бракує нашому проєкту. |

## Додана діагностика та зміни поведінки

- [app_config.h](C:/bluetooth-speaker/src/app_config.h:15): `BT_DIAGNOSTIC_VERBOSE_STACK_LOGS=0`, `BT_DIAGNOSTIC_MINIMAL_MODE=0`; перевірка, що stats interval не менший за 5 секунд. Tone/drain/bypass залишені й у фінальній збірці також `0`.
- [app_diagnostics.c](C:/bluetooth-speaker/src/app_diagnostics.c:1), [app_diagnostics.h](C:/bluetooth-speaker/src/app_diagnostics.h:1): публічні `esp_log_set_vprintf` та heap-failure hook, task watermarks. Немає patch SDK, приватного HCI hook чи заміни VHCI callback.
- [bluetooth_audio.c](C:/bluetooth-speaker/src/bluetooth_audio.c:558): прибрано колишній blanket `BT_APPL=ERROR`. Увімкнені WARN для HCI/APPL/A2D/AVDT. Normal показує перший і не частіше одного повторного рядка кожного з п'яти обраних fault types за 5 секунд; **кожне входження враховане у видимих totals/deltas**. Решта warning/error logs проходять без змін. Verbose показує також кожен повторний fault line. Коректні пакети не логуються, global DEBUG не вмикається.
- Minimal mode не ініціалізує AVRCP і не створює reconnect/RSSI/MAC-storage tasks. Не читає/не записує наш last-MAC key. Базова NVS initialization та штатне bonding storage самого Bluetooth зберігаються. У normal всі production-функції залишилися.
- [audio_pipeline.c](C:/bluetooth-speaker/src/audio_pipeline.c:480): збережено старі counters, додано timing/rate/buffer diagnostics. Мікросекундний monotonic timer замінює tick-based вимірювання gap. Pause не потрапляє в streaming gaps; mixed start/pause/rate intervals не рахуються як low-rate. Після receive timeout додатково перевіряється актуальний streaming state, щоб pause під час очікування не створювала хибний underflow. Stall тепер виявляється також коли після STARTED не було ще жодного callback.
- [main.c](C:/bluetooth-speaker/src/main.c:30), [status_led.c](C:/bluetooth-speaker/src/status_led.c:54), [i2s_output.c](C:/bluetooth-speaker/src/i2s_output.c:120), [src/CMakeLists.txt](C:/bluetooth-speaker/src/CMakeLists.txt:1): підключення діагностики та реєстрація task watermarks. Короткоживучі задачі зберігають число перед видаленням; stats не читає видалений task handle.
- [platformio.ini](C:/bluetooth-speaker/platformio.ini:1), новий [sdkconfig.esp32doit-devkit-v1-idf60](C:/bluetooth-speaker/sdkconfig.esp32doit-devkit-v1-idf60:1), README та цей звіт: відтворюваний A/B build і процедура перевірки.

GPIO, формат, sample rate, DMA, пріоритети, affinity, розмір StreamBuffer, prefetch threshold, tone, retry short writes та аварійний trim не змінювалися в цьому етапі. Підтвердженого transport fix не внесено. Наявні до цього етапу зміни `audio_pipeline.h` та generated config 6.1 збережені; generated config 6.1 залишився побайтово тим самим.

Семантика нових рядків (усі періодичні рядки раз на ≥5 секунд):

| Поле | Як читати |
| --- | --- |
| `BT fault logs total(+interval)` | Кількість відповідних **лог-подій**: incomplete ACL, continuation без START, SBC queue drop, sequence error, decode failure. Не є controller packet-loss counters. Зіставлення literal formats перевірене за SDK 6.0.1/6.1; майбутні SDK потребуватимуть повторної перевірки. Нулі потребують увімкнених compile/runtime WARN, що показує startup log. |
| `no_pcm_now_ms` | Поточний час від останнього callback, або від STARTED до першого callback. У suspended — 0. Не є тривалістю фізичної тиші на виході. |
| `gap_excess_over_30ms_total` | Сума частин PCM-free spans понад 30 ms, включно з незакритим span і стартовою затримкою, тільки під час STARTED. Наприклад, gap 180 ms додає 150 ms. Це показник нерівномірності, не оцінка втрачених PCM milliseconds. |
| `gap_buckets_total` | Завершені інтервали між callbacks під час одного STARTED: `[0,30)`, `[30,50)`, `[50,100)`, `[100,200]`, `(200,∞)` ms. Перший callback після STARTED не утворює bucket. Старі `gaps` рахують ≥35 ms; `last` — останній такий gap, `max` — найдовший завершений callback gap. |
| `buffer`, `buffer_min_playing` | Поточна fill і мінімум, спостережений writer перед читанням у playing. `-1` — ще немає вимірювання. Не включає initial prefetch; 0 саме по собі не доводить DMA underflow. |
| `underflows`, `prefetch_starts`, `resumes` | Underflow — невдале читання після 100 ms очікування під час streaming/playing, із поверненням у prefetch. Не лічильник DMA interrupts і не кожного короткого просідання. Prefetch starts включає один початковий вхід на stream start і кожен recovery; resumes — завершені prefetch. Для одного стабільного відтворення очікується `0 / 1 / 1`. |
| `rx_below_95pct_intervals=X/Y` | X повних started-інтервалів із RX <95% від negotiated rate, із Y придатних інтервалів. Для 44100 stereo межа 167580 B/s. Часткові інтервали з START/pause/rate change виключені; `full_started_interval` показує придатність поточного. |
| `Resources` | Free/minimum heap, найбільший 8-bit block, allocation failures після реєстрації public hook. Failure counter охоплює весь heap, не лише BT; сам по собі не визначає конкретний call site. Stack high-water marks — мінімальний залишок **у байтах** від старту задачі; `-1` — задача не стартувала / transient ще не завершився. Немає runtime HWM для закритих controller tasks. |

## A/B environments та результати build

| Environment | Platform / framework pin | Власний config / build directory |
| --- | --- | --- |
| `esp32doit-devkit-v1` (default) | `platformio/espressif32@7.1.3`, `platformio/framework-espidf@4.60100.0` → ESP-IDF 6.1.0 | `sdkconfig.esp32doit-devkit-v1`, `.pio/build/esp32doit-devkit-v1` |
| `esp32doit-devkit-v1-idf60` | `platformio/espressif32@7.0.1`, `platformio/framework-espidf@4.60001.0` → ESP-IDF 6.0.1 | `sdkconfig.esp32doit-devkit-v1-idf60`, `.pio/build/esp32doit-devkit-v1-idf60` |

Сумісність старої пари підтверджена [офіційним release 7.0.1](https://github.com/platformio/platform-espressif32/releases/tag/v7.0.1), його [package manifest](https://raw.githubusercontent.com/platformio/platform-espressif32/v7.0.1/platform.json), встановленими packages та успішною збіркою. Обидва використовують Xtensa toolchain `15.2.0+20251204`. Спільні board/framework/monitor/flash/partition settings винесені в `[env]`; GPIO спільні через той самий `app_config.h`.

Спроба `7.1.3 + framework 4.60001.0` завершилася **FAILED**: builder очікував `bootloader.memory.ld.in`, тоді як 6.0.1 має `bootloader.ld.in`. Замість patch installed packages для другого env використана офіційна сумісна платформа 7.0.1. Після цього build успішний. Перша спроба build у sandbox також потребувала доступу до PlatformIO locks/cache за межами workspace; подальші build виконані з таким доступом.

Порівняння common effective `CONFIG_BT*`, `CONFIG_FREERTOS*`, CPU, logging і power-management settings дало однакові значення; у 6.1 є новий `CONFIG_BT_CLASSIC_MAX_RECONNECT_ON_COLLISION=5`, відсутній у 6.0.1. Він стосується connect collision `0x0B`, не пояснює сам по собі втрати steady audio. Версії відрізняються також кодом SDK/controller, тому A/B локалізує залежність від версії, а не окремий файл.

Порівняно installed source обох версій: тіло `reassemble_and_dispatch()` однакове. У 6.1 є зміни RX validation/cleanup у HAL та guard checks у SBC decoder, але конкретного steady-state fix або регресії, що пояснюють цей лог, не доведено. Однаковість fragmenter не виключає відмінностей контролера чи решти стека.

Перевірки компіляції виконані окремо: normal 6.1; verbose 6.1; minimal 6.1; tone 6.1; normal 6.0.1. Усі пройшли. Після тимчасового перемикання flags вони повернуті в `0`, і обидва normal environments зібрані повторно. Повні build logs збережені локально у `.pio/idf61-*-build.log` і `.pio/idf60-normal-build.log`. `git diff` переглянуто, `git diff --check` проходить. Upload не виконувався.

| Остаточна перевірка | Команда | Результат / RAM / Flash, bytes |
| --- | --- | --- |
| 6.1 normal, усі flags 0 | `pio run -e esp32doit-devkit-v1` | SUCCESS / 55156 / 776845 |
| 6.1 verbose=1 | `pio run -e esp32doit-devkit-v1` | SUCCESS / 55156 / 776773 |
| 6.1 minimal=1 | `pio run -e esp32doit-devkit-v1` | SUCCESS / 55132 / 773805 |
| 6.1 tone=1 | `pio run -e esp32doit-devkit-v1` | SUCCESS / 19744 / 214845 |
| 6.0.1 normal, усі flags 0 | `pio run -e esp32doit-devkit-v1-idf60` | SUCCESS / 55724 / 755173 |

Попередня невдала комбінація: `pio run -e esp32doit-devkit-v1-idf60` при platform 7.1.3 — FAILED через bootloader template, 232.40 s; після pin 7.0.1 перший повний build — SUCCESS, 107.30 s. Перший normal 6.1 із діагностикою — SUCCESS, 81.16 s. Остаточні п'ять перевірок вище не містять compiler warnings/errors. Показник static RAM не включає всі runtime allocations Bluetooth/tasks; їх треба оцінювати за `Resources` на платі.

## Тести на платі

Для кожного запуску використати той самий локальний трек, ту саму ділянку 60–120 секунд, source volume, джерело, розташування, живлення та GPIO. Перезапуск плати перед кожним тестом обнулює cumulative stats. Оцінювати тільки повні `full_started_interval=yes` після первинного prefetch. Не поєднувати кілька діагностичних перемикачів в одному порівнянні.

| Тест на 6.1 | Tone / drain / bypass | Verbose | Minimal | Що відрізняє |
| --- | --- | --- | --- | --- |
| Normal | 0 / 0 / 0 | 0 | 0 | Повний production-набір із видимими агрегованими faults. |
| Verbose stack | 0 / 0 / 0 | 1 | 0 | Точна послідовність усіх fault lines. Якщо поведінка погіршилася тільки тут, синхронний UART сам додає навантаження. |
| Minimal A2DP | 0 / 0 / 0 | 0 | 1 | Підключити source вручну: reconnect, AVRCP і RSSI polling відсутні. Покращення вказує на вплив вимкненої групи, після чого функції треба ізолювати по одній. |

Змінити flags у `src/app_config.h`, зібрати потрібний env, **самостійно** завантажити саме його й запустити monitor. Команди нижче наведені для користувача; агент Upload не запускав:

```powershell
pio run -e esp32doit-devkit-v1
pio run -e esp32doit-devkit-v1 -t upload
pio device monitor -e esp32doit-devkit-v1 -b 115200
```

Для перевірки версії повернути всі flags у 0. Виконати **6.1 → 6.0.1 → 6.1** з тим самим source/треком/розташуванням. Для B:

```powershell
pio run -e esp32doit-devkit-v1-idf60
pio run -e esp32doit-devkit-v1-idf60 -t upload
pio device monitor -e esp32doit-devkit-v1-idf60 -b 115200
```

Якщо `pio` відсутній у PATH, замінити його на `& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"`. Environment завжди вказувати явно при Upload: IDE за замовчуванням використовує 6.1. Startup `Build: IDF=...` і flags мають відповідати тесту. NVS між тестами не стирати; за потреби перепідключити source вручну й зазначити це в логах.

Якщо 6.0.1 стабільний, а обидва запуски 6.1 повторюють помилку, це сильне свідчення залежності від SDK/platform pair. Якщо обидві версії однаково погані й minimal не допомагає, перевірити окремо phone/PC, потім розташування/орієнтацію антени при незмінній прошивці. Покращення лише з одним source або лише після зміни розташування розділяє ці гіпотези; близькість 60 cm сама не виключає RF/interference.

Allocation failures, падіння heap або малий запас наших стеків спрямовують до ресурсів. `sbc_queue_drop` при чистому `acl_incomplete` спрямовує до media queue/decoder/callback. Повторний `acl_incomplete` із нульовими allocation failures, нормальним heap та швидким callback залишає transport/controller/source пріоритетними для подальшої HCI-траси; ці лічильники не дають прямого профілю завантаження HCI task. Більший jitter buffer варто розглядати лише після стабільного довгострокового RX близько expected rate.

Повернути для кожного тесту: весь boot від `Build: IDF`, config/flags, controller/host init, pairing/reconnect/ACL/mode status, A2DP state, MTU й SBC config; далі **12–24 послідовні групи** `Audio stats` + `PCM timing` + `BT fault logs` + `Resources`, усі warning/error та underflow/prefetch між ними. Додати mode/env, source phone чи PC, тривалість, живлення/розташування й приблизний UART timestamp чутного переривання. Власні адреси MAC можна послідовно замінити однаковими псевдонімами.

Для 44100 Hz s16le stereo критерії: `rx_rate ≈176400 B/s`, `i2s_rate ≈rx_rate`, `underflows=0`, `prefetch_starts=1`, `resumes=1` для одного запуску потоку, `latency_trim=0`, `dropped=0`, `invalid_pcm=0`, `err=0`, `short=0` і відсутність повторних incomplete ACL events.
