"""CPU/sleep isolation and shared-algorithm checks. No firmware build or serial I/O."""
import hashlib
import json
from pathlib import Path
import re
import subprocess

from elftools.elf.elffile import ELFFile
from official_matrix import PROFILES, baseline, differences, settings, validate
from verify_clocked import body, digest, preprocess
from verify_official import compiled_contract


def check_matrix(project):
    snapshot = json.loads((project / 'docs/matrix-reference.json').read_text())
    original = baseline(project)
    for path, expected in snapshot['local_sdk_sources'].items():
        assert hashlib.sha256(Path(path).read_bytes()).hexdigest() == expected, path
    builder = snapshot['builder']
    assert hashlib.sha256(Path(builder['path']).read_bytes()).hexdigest() == builder['sha256']
    for name, expected in snapshot['source_sha256'].items():
        if name in ('src/CMakeLists.txt', 'src/control_official_i2s.c'):
            continue
        assert hashlib.sha256((project / name).read_bytes()).hexdigest() == expected, name

    # Every existing function except startup diagnostics is byte-identical C.
    for filename, functions in snapshot['official_function_source_sha256'].items():
        source = (project / 'src' / filename).read_text()
        for name, expected in functions.items():
            assert digest(body(source, name)) == expected, (filename, name)
    raw = (project / 'src/control_official_i2s.c').read_text()
    unchanged = raw.replace(body(raw, 'control_official_i2s_init'), '')
    unchanged = unchanged.replace('#include "esp_clk_tree.h"\n', '')
    unchanged = unchanged.replace('#ifndef CONTROL_OFFICIAL_ENV_NAME\n'
        '#error "Select the official environment through PlatformIO/CMake"\n#endif\n', '')
    assert digest(unchanged) == snapshot['official_service_without_boot_init_sha256'], \
        'Changes outside the boot diagnostic and its required declarations'
    raw_main = (project / 'src/control_official_main.c').read_text()
    assert not re.search(r'esp_bt_sleep_(enable|disable)|esp_pm_|esp_light_sleep|sniff|esp_clk_tree_src_set_freq_hz', raw + raw_main)
    assert raw.count('esp_clk_tree_src_get_freq_hz(') == 1
    assert 'esp_clk_tree_src_get_freq_hz(' in body(raw, 'control_official_i2s_init')

    result = []
    bodies = None
    for environment, (cpu, sleep, defaults) in PROFILES.items():
        build = project / '.pio/build' / environment
        values = settings(project / ('sdkconfig.' + environment))
        header = settings(build / 'config/sdkconfig.h', header=True)
        config_diff = validate(environment, values, original)
        validate(environment, header, original)
        assert not differences(values, header), 'sdkconfig.h differs from sdkconfig'
        description = json.loads((build / 'project_description.json').read_text())
        assert Path(description['config_file']).resolve() == project / ('sdkconfig.' + environment)
        cache = (build / 'CMakeCache.txt').read_text()
        assert 'CONTROL_OFFICIAL_ENV:UNINITIALIZED=' + environment in cache
        commands = json.loads((build / 'compile_commands.json').read_text())
        app = [e for e in commands if Path(e['file']).parent.resolve() == project / 'src']
        assert {Path(e['file']).name for e in app} == {'control_official_main.c', 'control_official_i2s.c'}
        for entry in app:
            assert '-DCONTROL_CMAKE_VARIANT=9' in entry['command']
            assert environment in entry['command'] and 'CONTROL_OFFICIAL_ENV_NAME' in entry['command']
            assert 'toolchain-xtensa-esp-elf' in entry['command']
        variant = environment.removeprefix('control-')
        preprocessed = {filename: preprocess(project, variant, filename)
                        for filename in snapshot['official_function_source_sha256']}
        current = {filename: {fn: body(preprocessed[filename], fn) for fn in functions}
                   for filename, functions in snapshot['official_function_source_sha256'].items()}
        if bodies is None:
            bodies = current
        else:
            assert current == bodies, 'Application algorithm differs after preprocessing: ' + environment
        boot = body(preprocessed['control_official_i2s.c'], 'control_official_i2s_init')
        assert f'const char *sleep_config = "{"enabled" if sleep else "disabled"}"' in boot
        assert 'CONTROL cpu_config_mhz=%u bt_modem_sleep_config=%s' in boot
        assert re.search(rf'\b{cpu}, sleep_config', boot), environment
        assert f'"{environment}"' in boot
        parameters = compiled_contract(project, environment, cpu, sleep)
        with (build / 'firmware.elf').open('rb') as handle:
            elf = ELFFile(handle)
            symbols = {s.name for s in elf.get_section_by_name('.symtab').iter_symbols()
                       if s['st_shndx'] != 'SHN_UNDEF'}
            rodata = b''.join(s.data() for s in elf.iter_sections()
                              if s['sh_type'] == 'SHT_PROGBITS' and 'rodata' in s.name)
        assert environment.encode() + b'\0' in rodata
        assert not any(other.encode() + b'\0' in rodata for other in PROFILES if other != environment)
        for token in (b'CONTROL env=%s variant=%s', b'official-i2s-reference\0',
                      b'cpu_config_mhz=%u bt_modem_sleep_config=%s',
                      b'cpu_runtime_hz=%', b'CONTROL ring=%u prefetch=%u',
                      b'task_core=unpinned', b'output_format=MSB official reference; no audible validation'):
            assert token in rodata, (environment, token)
        assert 'esp_clk_tree_src_get_freq_hz' in symbols
        assert not any(re.match(r'control_(?:spsc|stream|transport|i2s)_|[xv]StreamBuffer|clocked_', s)
                       for s in symbols), environment
        toolchain = Path.home() / '.platformio/packages/toolchain-xtensa-esp-elf/bin'
        for filename in ('control_official_i2s.c.o', 'control_official_main.c.o'):
            undefined = subprocess.check_output([str(toolchain / 'xtensa-esp32-elf-nm.exe'),
                '-u', str(build / 'src' / filename)], text=True)
            assert not re.search(r'esp_bt_sleep_|esp_pm_|esp_light_sleep|esp_clk_tree_src_set_freq_hz', undefined)
        result.append({'environment': environment, 'checks': 'PASS', 'cpu_config_mhz': cpu,
                       'bt_modem_sleep_config': 'enabled' if sleep else 'disabled',
                       'sdkconfig': 'sdkconfig.' + environment, 'defaults': defaults,
                       'build_directory': '.pio/build/' + environment, 'config_diff': config_diff,
                       'compiled_parameters': parameters, 'unique_banner_in_elf': True,
                       'hardware_test': 'NOT RUN'})

    # The old eight configurations must still match their original hashes/values.
    legacy_units = 0
    from verify_aligned import config_digest
    for variant, files in snapshot['legacy_preprocessed_sha256'].items():
        assert config_digest(project / ('sdkconfig.control-' + variant)) == \
            snapshot['sdkconfig_semantic_sha256'][variant], variant
        if variant == 'official-i2s-reference':
            continue  # Its only source change is the startup banner checked above.
        for filename, expected in files.items():
            assert digest(preprocess(project, variant, filename)) == expected, (variant, filename)
            legacy_units += 1
    sequence_path = project / '.pio/matrix-audit/build-sequence.json'
    build_isolation = 'NOT CHECKED (build audit file unavailable)'
    if sequence_path.is_file():
        sequence = json.loads(sequence_path.read_text())
        assert [r['environment'] for r in sequence] == list(PROFILES) + ['control-official-i2s-reference']
        assert all(r['exit_code'] == 0 and not r['unrelated_sdkconfig_files_changed'] for r in sequence)
        assert sequence[0]['settings'] == sequence[-1]['settings']
        # Prevent an old run's audit being used after generated configs change.
        for row in sequence[-4:]:
            assert row['settings'] == settings(project / ('sdkconfig.' + row['environment']))
        build_isolation = 'PASS'
    return {'checks': 'PASS', 'build_config_isolation': build_isolation,
            'old_preprocessed_units_unchanged': legacy_units, 'old_nine_sdkconfigs_unchanged': True,
            'official_main_source_unchanged': True, 'service_changed_only_at_startup': True,
            'all_nonstartup_function_bodies_identical_across_matrix': True,
            'runtime_sleep_override_absent': True, 'runtime_clock_switching_or_pm_locks_added': False,
            'public_cpu_clock_read_only_once_at_startup': True,
            'root_hardware_status': 'NOT RUN', 'elf_identity_required': False,
            'environments': result}


if __name__ == '__main__':
    print(json.dumps(check_matrix(Path(__file__).resolve().parents[1]), indent=2))
