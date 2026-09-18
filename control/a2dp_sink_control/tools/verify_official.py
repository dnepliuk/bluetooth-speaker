"""Read-only firmware/source audit of the locally derived IDF 6.1 reference.

Compiles a small config probe with the installed Xtensa compiler. Never builds
firmware, uploads or opens serial ports. Historical ELF identity is not required.
"""
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

from elftools.elf.elffile import ELFFile
from verify_clocked import body, digest, preprocess
from verify_aligned import config_digest


def symbol_bytes(path, name):
    with path.open('rb') as stream:
        elf = ELFFile(stream)
        symbol = elf.get_section_by_name('.symtab').get_symbol_by_name(name)[0]
        section = elf.get_section(symbol['st_shndx'])
        offset = symbol['st_value'] - section['sh_addr']
        return section.data()[offset:offset + symbol['st_size']]


def compiled_contract(project, environment='control-official-i2s-reference', cpu=160, sleep=True):
    build = project / '.pio/build' / environment
    entries = json.loads((build / 'compile_commands.json').read_text())
    entry = next(e for e in entries if Path(e['file']).name == 'control_official_i2s.c')
    probe = project / '.pio/official-audit' / (environment + '-contract.o')
    probe.parent.mkdir(exist_ok=True)
    command = re.sub(r' -o .*? -c .*$', ' -DCONTROL_VARIANT=9 -x c -c - -o "' +
                     probe.as_posix() + '"', entry['command'])
    assert command != entry['command']
    fields = ['dma_desc_num', 'dma_frame_num', 'intr_priority']
    expected = {'OFFICIAL_RING_BYTES': 32768, 'OFFICIAL_PREFETCH_BYTES': 20480,
                'OFFICIAL_WRITE_UPTO': 1440, 'OFFICIAL_READ_TIMEOUT_MS': 20,
                'OFFICIAL_WRITER_STACK': 4096, 'OFFICIAL_WRITER_PRIORITY': 22,
                'OFFICIAL_STATS_PERIOD_MS': 5000, 'OFFICIAL_BCK': 26,
                'OFFICIAL_WS': 25, 'OFFICIAL_DATA': 22,
                'CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ': cpu, 'CONFIG_BTDM_CTRL_MODEM_SLEEP': int(sleep)}
    source = '#include <stddef.h>\n#include "freertos/FreeRTOS.h"\n#include "driver/i2s_std.h"\n'
    source += '#include "control_official_i2s.h"\n'
    source += '#ifndef CONFIG_BTDM_CTRL_MODEM_SLEEP\n#define CONFIG_BTDM_CTRL_MODEM_SLEEP 0\n#endif\n'
    source += 'const i2s_chan_config_t probe_channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);\n'
    source += 'const unsigned probe_offsets[] = {' + ','.join(
        'offsetof(i2s_chan_config_t, ' + f + ')' for f in fields) + '};\n'
    source += '\n'.join(f'_Static_assert({k} == {v}, "{k}");' for k, v in expected.items())
    result = subprocess.run(command, cwd=entry['directory'], input=source,
                            text=True, capture_output=True)
    assert result.returncode == 0, result.stderr
    channel = symbol_bytes(probe, 'probe_channel')
    offsets = struct.unpack('<3I', symbol_bytes(probe, 'probe_offsets'))
    actual = {f: struct.unpack_from('<I', channel, offset)[0] for f, offset in zip(fields, offsets)}
    assert actual == {'dma_desc_num': 6, 'dma_frame_num': 240, 'intr_priority': 0}, actual
    return dict(expected, **actual)


def check_official(project):
    reference = json.loads((project / 'docs/official-reference.json').read_text())
    units = 0
    for variant, files in reference['legacy_preprocessed_sha256'].items():
        for filename, expected in files.items():
            assert digest(preprocess(project, variant, filename)) == expected, (variant, filename)
            units += 1
        assert config_digest(project / ('sdkconfig.control-' + variant)) == \
            reference['sdkconfig_semantic_sha256'][variant], variant
    for name, expected in reference['source_sha256'].items():
        if name in ('src/control_variant.h', 'src/CMakeLists.txt'):
            continue  # Selector/source-list extension; preprocessing checked above.
        assert hashlib.sha256((project / name).read_bytes()).hexdigest() == expected, name
    idf = Path(reference['normative_local_idf']['root'])
    for name, expected in reference['normative_local_idf']['files'].items():
        assert hashlib.sha256((idf / name).read_bytes()).hexdigest() == expected, name
    assert config_digest(project / 'sdkconfig.control-official-i2s-reference') == \
        config_digest(project / 'sdkconfig.control-spsc-dma-aligned'), 'Full SDK config differs'

    variant = 'official-i2s-reference'
    service = preprocess(project, variant, 'control_official_i2s.c')
    main = preprocess(project, variant, 'control_official_main.c')
    raw_service = (project / 'src/control_official_i2s.c').read_text()
    raw_main = (project / 'src/control_official_main.c').read_text()
    previous_main = (project / 'src/main.c').read_text()
    # Exact reuse of pairing; exact init after excluding the variant-specific hooks.
    assert body(raw_main, 'gap_callback') == body(previous_main, 'gap_callback')
    old_init = body(previous_main, 'app_main')
    old_init = re.sub(r'#if CONTROL_HAS_TRANSPORT.*?#endif', '', old_init, flags=re.S)
    old_init = re.sub(r'    if \(xTaskCreate\(stats_task,.*?\n    }\n', '', old_init, flags=re.S)
    old_init = re.sub(r'#if CONTROL_HAS_STREAM.*?#endif', '', old_init, flags=re.S)
    new_init = body(raw_main, 'app_main')
    new_init = re.sub(r'    ESP_ERROR_CHECK\(control_official_i2s_init\(\)\);', '', new_init)
    new_init = re.sub(r'    ESP_ERROR_CHECK\(esp_a2d_sink_register_data_callback\(official_pcm_data_callback\)\);', '', new_init)
    assert re.sub(r'\s+', '', old_init) == re.sub(r'\s+', '', new_init), 'Bluetooth init changed'

    callback = body(main, 'official_pcm_data_callback')
    assert re.sub(r'\s+', '', callback) == \
        'staticvoidofficial_pcm_data_callback(constuint8_t*data,uint32_tlen){control_official_i2s_output(data,len);}'
    hot = '\n'.join(body(service, n) for n in ('control_official_i2s_output', 'official_data_output', 'count_add'))
    assert not re.search(r'i2s_channel_|esp_timer_|esp_log\(|[mv]alloc\(|free\(|vTask|portENTER|spinlock|uint64_t|int64_t', hot)
    assert not re.search(r'\b(?:for|while)\s*\(', hot), 'Callback must not spin or wait'
    assert 'xRingbufferSend(s_ring, (void *)data, size, (TickType_t)0)' in hot
    assert 'return done ? size : 0;' in hot
    assert 'memcpy' not in hot and 'xSemaphoreTake' not in hot
    writer = body(service, 'official_writer_task')
    assert 'i2s_channel_write(s_tx, data, item_size,' in writer
    assert len(re.findall(r'\bi2s_channel_write\(', writer)) == 1
    assert 'xRingbufferReceiveUpTo(s_ring, &item_size,' in writer and '240 * 6' in writer
    assert 'vRingbufferReturnItem(s_ring, data)' in writer
    assert not any(token in writer for token in ('vTaskDelay', 'memset', 'memcpy', '4096', '3840'))
    for token in ('written != item_size', 'err != 0', 'UNDERFLOWS', 'PREFETCHING'):
        assert token in writer, token
    for token in ('xRingbufferReset', 'xTaskCreatePinnedToCore', 'control_transport_',
                  'control_spsc_', 'control_stream_', 'clocked_', 'memset(', 'memcpy('):
        assert token not in raw_service, token
    assert 'xTaskCreate(official_writer_task, "BtI2STask", OFFICIAL_WRITER_STACK, NULL,' in raw_service
    assert 'xTaskCreate(official_stats_task, "official_stats", 4096, NULL, 1, NULL)' in raw_service
    assert 'I2S_STD_MSB_SLOT_DEFAULT_CONFIG' in body(raw_service, 'control_official_i2s_open')
    assert 'chan_cfg.auto_clear = true;' in raw_service
    assert not re.search(r'chan_cfg\.(?:dma_desc_num|dma_frame_num|intr_priority)\s*=', raw_service)
    # Stop joins writer before disabling/deleting I2S or freeing ring resources.
    stop = body(raw_service, 'control_official_i2s_stop')
    assert stop.index('xSemaphoreTake(s_writer_exited') < stop.index('i2s_channel_disable') < stop.index('vRingbufferDelete')

    build = project / '.pio/build/control-official-i2s-reference'
    with (build / 'firmware.elf').open('rb') as stream:
        elf = ELFFile(stream)
        symbols = {s.name for s in elf.get_section_by_name('.symtab').iter_symbols()
                   if s['st_shndx'] != 'SHN_UNDEF'}
        rodata = b''.join(s.data() for s in elf.iter_sections()
                          if s['sh_type'] == 'SHT_PROGBITS' and 'rodata' in s.name)
    banned = re.compile(r'(?:control_(?:transport|spsc|stream|i2s)_|[xv]StreamBuffer|clocked_)')
    assert not any(banned.match(s) for s in symbols), sorted(s for s in symbols if banned.match(s))
    # Bluedroid itself links AVRCP mask helpers and 64-bit atomic helpers even
    # in drain-only baseline. Check app call paths/object code, not SDK internals.
    assert 'esp_avrc_' not in raw_main + raw_service
    with (project / '.pio/build/control-baseline/firmware.elf').open('rb') as stream:
        baseline_elf = ELFFile(stream)
        baseline_avrc = {s.name for s in baseline_elf.get_section_by_name('.symtab').iter_symbols()
                         if s.name.startswith('esp_avrc_') and s['st_shndx'] != 'SHN_UNDEF'}
    assert {s for s in symbols if s.startswith('esp_avrc_')} <= baseline_avrc
    for name in ('xRingbufferSend', 'xRingbufferReceiveUpTo', 'i2s_channel_write',
                 'official_writer_task', 'official_pcm_data_callback', 'official_stats_task'):
        assert name in symbols, name
    for literal in (b'official-i2s-reference\0', b'CONTROL variant=%s',
                    b'CONTROL source=ESP-IDF-6.1 a2dp_sink_stream',
                    b'CONTROL ring=%u prefetch=%u', b'task_core=unpinned',
                    b'CONTROL output_format=MSB official reference; no audible validation',
                    b'OFFICIAL stats:', b'Faital A2DP Control'):
        assert literal in rodata, literal
    toolchain = Path.home() / '.platformio/packages/toolchain-xtensa-esp-elf/bin'
    assembly = subprocess.check_output([str(toolchain / 'xtensa-esp32-elf-objdump.exe'),
        '-dr', str(build / 'src/control_official_i2s.c.o')], text=True)
    assert not re.search(r'__atomic_|__sync_|s32c1i', assembly), 'Atomics must be inline load/store'
    return {'checks': 'PASS', 'previous_preprocessed_units_unchanged': units,
            'previous_eight_sdkconfigs_unchanged': True, 'old_transport_sources_unchanged': True,
            'normative_idf_sources_unchanged': True, 'full_sdkconfig_matches_aligned': True,
            'bluetooth_init_and_pairing_preserved': True,
            'compiled_parameters': compiled_contract(project),
            'callback_bounded_no_timer_lock_alloc_write_log': True,
            'atomics_inline_load_store_no_helpers_or_rmw': True,
            'custom_transport_symbols_absent': True, 'cooperative_close_order': True,
            'hardware': 'NOT RUN in current matrix; historical reference FAIL reported by user',
            'elf_identity_required': False}


if __name__ == '__main__':
    print(json.dumps(check_official(Path(__file__).resolve().parents[1]), indent=2))
