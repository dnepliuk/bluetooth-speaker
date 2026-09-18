"""Read-only compiler/ELF checks against the snapshot taken before variant 7."""

import hashlib
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

from elftools.elf.elffile import ELFFile
from verify_clocked import body, digest, preprocess


def check_spsc(project, strict_elf=False):
    reference = json.loads((project / 'docs/spsc-reference.json').read_text())
    units = 0
    for variant, files in reference['legacy_preprocessed_sha256'].items():
        for name, expected in files.items():
            assert digest(preprocess(project, variant, name)) == expected, (variant, name)
            units += 1
    binary_results = {}
    for variant, expected in reference['elf_sections'].items():
        path = project / '.pio/build' / ('control-' + variant) / 'firmware.elf'
        with path.open('rb') as handle:
            elf = ELFFile(handle)
            sections = {s.name: hashlib.sha256(s.data()).hexdigest()
                        for s in elf.iter_sections() if s['sh_type'] == 'SHT_PROGBITS'
                        and s['sh_flags'] & 2 and s.name != '.flash.appdesc'}
        changed = {name: {'expected': expected.get(name), 'actual': sections.get(name)}
                   for name in sorted(set(expected) | set(sections))
                   if expected.get(name) != sections.get(name)}
        binary_results[variant] = {
            'checks': 'FAIL' if changed else 'PASS',
            'compared_sections': len(expected), 'changed_sections': changed,
        }

    name = 'spsc-pipeline'
    main = preprocess(project, name, 'main.c')
    assert main.replace('"spsc-pipeline"', '"clocked-pipeline"') == preprocess(
        project, 'clocked-pipeline', 'main.c'), 'Bluetooth/PCM callback changed'
    assert preprocess(project, name, 'control_i2s.c') == preprocess(
        project, 'clocked-pipeline', 'control_i2s.c'), 'I2S config/driver changed'
    transport = preprocess(project, name, 'control_transport.c')
    clocked = preprocess(project, 'clocked-pipeline', 'control_transport.c')
    for fn in ('clocked_write', 'clocked_drain', 'control_transport_callback_done',
               'control_transport_set_streaming', 'control_transport_set_format'):
        assert body(transport, fn) == body(clocked, fn), fn
    for fn in ('clocked_loop', 'clocked_read'):
        normalized = body(transport, fn).replace('control_spsc_consumer_fill()',
                                                'control_stream_fill()')
        normalized = normalized.replace('control_spsc_receive(data, size)',
                                        'control_stream_receive(data, size, 0)')
        assert normalized == body(clocked, fn), 'Reference writer changed: ' + fn
    ring = preprocess(project, name, 'control_spsc.c')
    # idf_additions.h unconditionally declares StreamBuffer APIs, even in the
    # baseline. Check actual application function bodies, not SDK declarations.
    api_pattern = r'\b(?:[xv]StreamBuffer\w*|control_stream_\w*)\s*\('
    declarations = len(re.findall(api_pattern, main))
    for filename, source in (('main.c', main), ('control_transport.c', transport)):
        original = (project / 'src' / filename).read_text(encoding='utf-8')
        functions = re.findall(r'(?:static )?(?:void|size_t) (\w+)\([^)]*\)\s*\{', original)
        for fn in functions:
            if re.search(r'(?:void|size_t) ' + fn + r'\([^)]*\)\s*\{', source):
                assert not re.search(api_pattern, body(source, fn)), (filename, fn)
    assert not re.search(api_pattern, ring), 'Ring includes StreamBuffer declarations or calls'
    # Ring is bounded: copies, scalar arithmetic, timer and atomic load/stores only.
    for fn in ('control_spsc_send', 'control_spsc_receive'):
        code = body(ring, fn)
        assert not re.search(r'\b(?:while|for)\s*\(', code), fn
        assert len(re.findall(r'\bmemcpy\(', code)) == 2, fn
        assert not re.search(r'portENTER|TaskNotify|Semaphore|Queue|malloc|free\(', code), fn
    build = project / '.pio/build/control-spsc-pipeline'
    compiler = Path.home() / '.platformio/packages/toolchain-xtensa-esp-elf/bin'
    nm = compiler / 'xtensa-esp32-elf-nm.exe'
    objdump = compiler / 'xtensa-esp32-elf-objdump.exe'
    obj = build / 'src/control_spsc.c.o'
    undefined = subprocess.check_output([str(nm), '-u', str(obj)], text=True)
    names = {line.split()[-1] for line in undefined.splitlines()}
    assert names <= {'memcpy', 'esp_timer_get_time'}, undefined
    assembly = subprocess.check_output([str(objdump), '-d', str(obj)], text=True)
    assert 'memw' in assembly, 'Missing ESP32 memory barriers'
    assert not re.search(r'__atomic_|__sync_|s32c1i', assembly), \
        'Ring must use inline load/store atomics without RMW retry loops'
    with (build / 'firmware.elf').open('rb') as handle:
        elf = ELFFile(handle)
        symbols = list(elf.get_section_by_name('.symtab').iter_symbols())
        pcm = next(s for s in symbols if s.name == 's_pcm')
        assert pcm['st_size'] == 32768 and pcm['st_value'] % 4 == 0
        assert 0x3FFAE000 <= pcm['st_value'] < 0x40000000 - 32768, \
            'Ring is outside ESP32 internal DRAM'
        assert not any(re.match(r'(?:[xv]StreamBuffer|control_stream_)', s.name)
                       for s in symbols), 'StreamBuffer found in SPSC ELF'
        ram_address = hex(pcm['st_value'])
    return {
        # Historical hashes remain visible. Variant 9's requested acceptance
        # checks source/config/symbol semantics; ELF identity is optional.
        'checks': 'FAIL' if strict_elf and any(r['checks'] == 'FAIL' for r in binary_results.values()) else 'PASS',
        'strict_elf_checks': 'PASS' if all(r['checks'] == 'PASS' for r in binary_results.values()) else 'FAIL',
        'strict_elf_required': strict_elf,
        'spsc_code_and_isolation_checks': 'PASS',
        'previous_translation_units_unchanged': units,
        'previous_allocated_elf_section_comparison': binary_results,
        'elf_exclusion': '.flash.appdesc only (build date/time and ELF hash)',
        'bluetooth_and_pcm_callback_identical': True,
        'clocked_writer_identical_after_buffer_substitution': True,
        'i2s_translation_unit_identical': True,
        'no_streambuffer_calls_in_preprocessed_application_functions': True,
        'no_streambuffer_symbols_in_elf': True,
        'sdk_streambuffer_declarations_in_main_preprocessing': declarations,
        'ring_capacity': 32768, 'ring_internal_dram_address': ram_address,
        'ring_undefined_symbols': sorted(names), 'xtensa_memw_present': True,
        'ring_atomic_helpers_or_retry_loops': False,
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--strict-elf', action='store_true', help='Also require historical ELF byte identity')
    args = parser.parse_args()
    result = check_spsc(Path(__file__).resolve().parents[1], strict_elf=args.strict_elf)
    print(json.dumps(result, indent=2))
    sys.exit(0 if result['checks'] == 'PASS' else 1)
