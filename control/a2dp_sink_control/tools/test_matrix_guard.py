"""Exercise the real read-only SCons guard with isolated stale-config fixtures."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import runpy
import sys
import tempfile
import types
from unittest.mock import patch

from official_matrix import PROFILES, SLEEP_KEYS, baseline, validate


def check_guard():
    project = Path(__file__).resolve().parents[1]
    original = baseline(project)
    checked = 0
    work = project / '.pio/matrix-audit'
    work.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=work) as temp:
        root = Path(temp)
        (root / 'docs').mkdir()
        (root / 'docs/matrix-reference.json').write_bytes((project / 'docs/matrix-reference.json').read_bytes())
        for name, (cpu, sleep, _) in PROFILES.items():
            correct = dict(original)
            correct.update({key: str(int(sleep)) for key in SLEEP_KEYS})
            for prefix in ('CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ', 'CONFIG_ESP32_DEFAULT_CPU_FREQ'):
                for rate in (160, 240):
                    correct[prefix + '_' + str(rate)] = str(int(cpu == rate))
            correct['CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ'] = str(cpu)
            correct['CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ'] = str(cpu)
            validate(name, correct, original)
            config = root / ('sdkconfig.' + name)
            build = root / '.pio/build' / name
            (build / 'config').mkdir(parents=True)
            header = build / 'config/sdkconfig.h'
            for fault in ('none', 'cpu', 'sleep', 'unrelated_pm', 'header', 'shared_path', 'shared_build'):
                values, header_values = dict(correct), dict(correct)
                if fault == 'cpu': values['CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ'] = str(400 - cpu)
                if fault == 'sleep': values['CONFIG_BTDM_CTRL_MODEM_SLEEP'] = str(int(not sleep))
                if fault == 'unrelated_pm': values['CONFIG_PM_ENABLE'] = '1'
                if fault == 'header': header_values['CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ'] = str(400 - cpu)
                config.write_text(''.join(f'{k}={v}\n' for k, v in values.items()))
                header.write_text(''.join(f'#define {k} {v}\n' for k, v in header_values.items()))
                before = (config.read_bytes(), header.read_bytes())

                class Environment:
                    def subst(self, key):
                        return {'$PROJECT_DIR': str(root), '$PIOENV': name,
                                '$BUILD_DIR': str(build if fault != 'shared_build' else build.parent / 'shared')}[key]
                    def BoardConfig(self):
                        return {'build.esp-idf.sdkconfig_path': config.name if fault != 'shared_path' else 'sdkconfig.shared'}
                    def GetOption(self, key): return False
                    def Exit(self, code): raise SystemExit(code)

                script_module = types.ModuleType('SCons.Script')
                script_module.COMMAND_LINE_TARGETS = []
                output, status = io.StringIO(), 0
                with patch.dict(sys.modules, {'SCons.Script': script_module}), contextlib.redirect_stdout(output):
                    try:
                        runpy.run_path(str(project / 'tools/matrix_config_guard.py'),
                                       init_globals={'env': Environment(), 'Import': lambda name: None})
                    except SystemExit as exc:
                        status = exc.code
                assert status == (0 if fault == 'none' else 1), (name, fault, output.getvalue())
                assert before == (config.read_bytes(), header.read_bytes()), 'Guard modified fixture'
                checked += 1
    return {'checks': 'PASS', 'cases': checked,
            'coverage': 'All four profiles: valid, stale CPU/sleep/header, unrelated PM, shared config/build paths',
            'configuration_files_modified_by_guard': False}


if __name__ == '__main__':
    print(json.dumps(check_guard(), indent=2))
