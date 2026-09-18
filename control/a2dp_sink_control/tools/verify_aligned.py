"""Verify the sole functional 7 -> 8 change and keep strict ELF results separate.

Read-only: uses existing builds and the immutable pre-variant-8 snapshot.
No Build/Upload/Monitor. Historical ELF mismatches are reported separately;
--strict-elf additionally makes byte identity an exit-status requirement.
"""

import hashlib
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

from elftools.elf.elffile import ELFFile
from verify_clocked import body, digest, preprocess


def allocated_sections(path):
    with path.open('rb') as handle:
        elf = ELFFile(handle)
        return {s.name: hashlib.sha256(s.data()).hexdigest()
                for s in elf.iter_sections() if s['sh_type'] == 'SHT_PROGBITS'
                and s['sh_flags'] & 2 and s.name != '.flash.appdesc'}


def config_digest(path):
    enabled = {}
    for line in path.read_text(encoding='utf-8').splitlines():
        match = re.fullmatch(r'(CONFIG_\w+)=(.*)', line)
        if match and match[2] != 'n':
            enabled[match[1]] = match[2]
    return digest(json.dumps(enabled, sort_keys=True, separators=(',', ':')))


def audio_contract(project, variant, output):
    build = project / '.pio/build' / ('control-' + variant)
    commands = json.loads((build / 'compile_commands.json').read_text())
    entry = next(x for x in commands if Path(x['file']).resolve() == project / 'src/main.c')
    selector = re.search(r'-DCONTROL_CMAKE_VARIANT=(\d+)', entry['command'])[1]
    command = re.sub(r' -o .*? -c .*$', ' -fsyntax-only -x c -', entry['command'])
    assert command != entry['command'], 'Cannot replace compile command input'
    command += ' -DCONTROL_VARIANT=' + selector
    expected = {
        'CONTROL_CHUNK_BYTES': output, 'CONTROL_FRAME_BYTES': 4,
        'CONTROL_STREAM_BYTES': 32768, 'CONTROL_PREFILL_BYTES': 8192,
        'CONTROL_DMA_DESCRIPTORS': 3, 'CONTROL_DMA_FRAMES': 960,
        'CONTROL_WRITER_PRIORITY': 22, 'CONTROL_WRITER_CORE': 1,
        'CONTROL_WRITER_STACK': 8192, 'CONTROL_SAMPLE_RATE': 44100,
        'CONTROL_WRITE_TIMEOUT_MS': 1000,
    }
    code = '#include "freertos/FreeRTOS.h"\n#include "control_variant.h"\n#include "control_audio_config.h"\n'
    code += '\n'.join(f'_Static_assert({key} == {value}, "{key}");'
                      for key, value in expected.items())
    result = subprocess.run(command, cwd=entry['directory'], input=code,
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return expected


def check_aligned(project, strict_elf=False):
    reference = json.loads((project / 'docs/aligned-reference.json').read_text())
    units = 0
    binary = {}
    for variant, files in reference['legacy_preprocessed_sha256'].items():
        for name, expected in files.items():
            assert digest(preprocess(project, variant, name)) == expected, (variant, name)
            units += 1
        assert config_digest(project / ('sdkconfig.control-' + variant)) == \
            reference['sdkconfig_semantic_sha256'][variant], ('Previous sdkconfig changed', variant)
        actual = allocated_sections(project / '.pio/build' / ('control-' + variant) / 'firmware.elf')
        expected = reference['elf_sections'][variant]
        changed = {key: {'expected': expected.get(key), 'actual': actual.get(key)}
                   for key in sorted(set(expected) | set(actual)) if expected.get(key) != actual.get(key)}
        binary[variant] = {'checks': 'FAIL' if changed else 'PASS', 'changed_sections': changed}
    for name, expected in reference['unchanged_source_sha256'].items():
        assert digest((project / name).read_text(encoding='utf-8')) == expected, name

    old, new = 'spsc-pipeline', 'spsc-dma-aligned'
    contracts = {v: audio_contract(project, v, output) for v, output in ((old, 4096), (new, 3840))}
    assert {k for k in contracts[old] if contracts[old][k] != contracts[new][k]} == {'CONTROL_CHUNK_BYTES'}
    assert config_digest(project / ('sdkconfig.control-' + old)) == config_digest(
        project / ('sdkconfig.control-' + new)), 'Full sdkconfig semantics differ'
    main = preprocess(project, new, 'main.c')
    assert main.replace('"spsc-dma-aligned"', '"spsc-pipeline"') == preprocess(project, old, 'main.c')
    assert preprocess(project, new, 'control_i2s.c') == preprocess(project, old, 'control_i2s.c')
    rings = [preprocess(project, v, 'control_spsc.c') for v in (old, new)]
    # Only the chunk-specific compile-time assertion differs in the ring TU.
    assert re.sub(r'_Static_assert\(.*?;\n', '', rings[0], flags=re.S) == \
        re.sub(r'_Static_assert\(.*?;\n', '', rings[1], flags=re.S), 'Ring/producer/consumer changed'
    before = preprocess(project, old, 'control_transport.c')
    after = preprocess(project, new, 'control_transport.c')
    banner_lines = [line for line in after.splitlines(keepends=True) if 'writer DMA alignment:' in line]
    assert len(banner_lines) == 1, 'Expected one extra boot log only'
    boot = banner_lines[0]
    assert boot.strip() in body(after, 'control_transport_init'), 'Alignment log must run only at init'
    for field in ('output_bytes=%u', 'dma_descriptor_bytes=%u', 'dma_frames=%u',
                  'frame_bytes=%u', 'writer_dma_aligned=yes', '3840U, 960U * 4U, 960U, 4U'):
        assert field in boot, field
    # Excluding the diagnostic string, this statement may only call the logger.
    tokens = re.sub(r'"(?:[^"\\]|\\.)*"', '""', boot)
    calls = set(re.findall(r'\b(\w+)\s*\(', tokens))
    assert calls <= {'if', 'while', 'esp_log', 'esp_log_timestamp'}, calls
    normalized = after.replace(boot, '')
    normalized = re.sub(r'\b3840U\b', '4096U', normalized)
    assert normalized == before, 'Transport differs beyond block size and the single boot diagnostic'
    loop, write = body(after, 'clocked_loop'), body(after, 'clocked_write')
    assert 'uint8_t chunk[3840U]' in loop and '4096' not in loop
    assert 'vTaskDelay' not in loop and '4096' not in write
    assert 'const size_t remaining = 3840U - offset' in write
    assert 'control_i2s_write(chunk + offset, remaining, &written)' in write
    assert 'if (written == 0) vTaskDelay(1)' in write  # Existing error backoff only.
    assert 'fill >= 8192U' in loop
    assert '3U * 960U * 4U' in loop
    assert 'uint8_t chunk[4096U]' in body(before, 'clocked_loop')
    assert body(after, 'control_transport_send') == body(before, 'control_transport_send')
    assert body(after, 'control_transport_log_stats') == body(before, 'control_transport_log_stats')

    # Prove the unchanged low-level C also generates identical loadable object
    # code/data, independently of linker addresses and final image metadata.
    for filename in ('control_spsc.c.o', 'control_i2s.c.o'):
        paths = [project / '.pio/build' / ('control-' + v) / 'src' / filename for v in (old, new)]
        assert allocated_sections(paths[0]) == allocated_sections(paths[1]), filename
    build = project / '.pio/build/control-spsc-dma-aligned'
    with (build / 'firmware.elf').open('rb') as handle:
        elf = ELFFile(handle)
        symbols = list(elf.get_section_by_name('.symtab').iter_symbols())
        rodata = b''.join(s.data() for s in elf.iter_sections()
                          if s['sh_type'] == 'SHT_PROGBITS' and 'rodata' in s.name)
        assert b'spsc-dma-aligned\0' in rodata and b'CONTROL variant=%s' in rodata
        assert b'writer_dma_aligned=yes' in rodata and b'dma_descriptor_bytes=%u' in rodata
        assert not any(re.match(r'(?:[xv]StreamBuffer|control_stream_)', s.name) for s in symbols)
        pcm = next(s for s in symbols if s.name == 's_pcm')
        assert pcm['st_size'] == 32768 and pcm['st_value'] % 4 == 0
        assert 0x3FFAE000 <= pcm['st_value'] < 0x40000000 - 32768
    return {
        'checks': 'FAIL' if strict_elf and any(v['checks'] == 'FAIL' for v in binary.values()) else 'PASS',
        'strict_elf_required': strict_elf,
        'source_preprocessed_checks': 'PASS',
        'semantic_checks': 'PASS', 'previous_preprocessed_units_unchanged': units,
        'previous_seven_sdkconfigs_unchanged': True,
        'sdkconfig_7_and_8_semantically_identical': True,
        'audio_contracts': contracts, 'sole_functional_difference': 'CONTROL_CHUNK_BYTES: 4096 -> 3840',
        'ring_and_i2s_loadable_object_sections_identical': True,
        'transport_identical_except_chunk_and_one_boot_log': True,
        'pcm_callback_producer_and_aggregate_logs_identical': True,
        'no_streambuffer_in_aligned_elf': True,
        'strict_elf_checks': 'PASS' if all(v['checks'] == 'PASS' for v in binary.values()) else 'FAIL',
        'previous_allocated_elf_section_comparison': binary,
        'elf_exclusion': '.flash.appdesc only; mismatch remains FAIL, without implying a source change',
        'hardware': 'Historical aligned FAIL reported by user; no hardware run by agent',
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--strict-elf', action='store_true', help='Also require historical ELF byte identity')
    args = parser.parse_args()
    result = check_aligned(Path(__file__).resolve().parents[1], strict_elf=args.strict_elf)
    print(json.dumps(result, indent=2))
    sys.exit(0 if result['checks'] == 'PASS' else 1)
