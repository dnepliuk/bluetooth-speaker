# Faital A2DP Control

Незалежний контроль Bluetooth Classic A2DP Sink для DOIT ESP32 DEVKIT V1
(ESP32/WROOM-32), USB-живлення, без периферії. Приймає SBC через внутрішній
декодер Bluedroid і відкидає PCM. Звуку на виході немає.

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
зберігається тільки тут, у `sdkconfig.a2dp_sink_control`.

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

```powershell
cd C:\bluetooth-speaker\control\a2dp_sink_control
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -t upload
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" device monitor -b 115200
```

Upload і Monitor запускаються вручну. Якщо підключено кілька COM-пристроїв,
додайте потрібний `--upload-port COMx` або `--port COMx` відповідно.
Сполучіть source з `Faital A2DP Control`, увімкніть безперервний трек і
порівняйте кілька п'ятисекундних вікон за однакових умов з основним застосунком.
Якщо source зберіг стару назву/профілі тієї самої ESP32, видаліть старе
сполучення на source і сполучіть заново.

Скорочений **ілюстративний**, не виміряний UART-лог; MAC і числа умовні:

```text
I (...) CONTROL: build=a2dp_sink_control (drain-only), device=Faital A2DP Control, ESP-IDF=6.1.0
I (...) CONTROL: Bluetooth Classic controller enabled
I (...) CONTROL: Bluedroid host enabled
I (...) CONTROL: A2DP Sink profile=initialized
I (...) CONTROL: ready: discoverable as Faital A2DP Control; PCM drain-only, audio_state=stopped
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
