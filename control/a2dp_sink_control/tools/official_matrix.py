"""CPU/sleep matrix contracts shared by the build guard and verifier."""
import hashlib
import json
from pathlib import Path
import re

PROFILES = {
    'control-official-i2s-reference': (160, True, 'sdkconfig.official-160-sleep.defaults'),
    'control-official-160-nosleep': (160, False, 'sdkconfig.official-160-nosleep.defaults'),
    'control-official-240-sleep': (240, True, 'sdkconfig.official-240-sleep.defaults'),
    'control-official-240-nosleep': (240, False, 'sdkconfig.official-240-nosleep.defaults'),
}
CPU_KEYS = {
    'CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ', 'CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160',
    'CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240', 'CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ',
    'CONFIG_ESP32_DEFAULT_CPU_FREQ_160', 'CONFIG_ESP32_DEFAULT_CPU_FREQ_240',
}
SLEEP_KEYS = {
    'CONFIG_BTDM_CTRL_MODEM_SLEEP', 'CONFIG_BTDM_CONTROLLER_MODEM_SLEEP',
    'CONFIG_BTDM_CTRL_MODEM_SLEEP_MODE_ORIG', 'CONFIG_BTDM_CTRL_LPCLK_SEL_MAIN_XTAL',
}
ALLOWED_KEYS = CPU_KEYS | SLEEP_KEYS


def settings(path, header=False):
    """Normalize boolean y to 1, disabled/absent to 0; retain numeric/string values."""
    result = {}
    for line in Path(path).read_text(encoding='utf-8-sig').splitlines():
        match = re.fullmatch(r'#define (CONFIG_\w+)\s+(.+)', line) if header else \
            re.fullmatch(r'(CONFIG_\w+)=(.*)', line)
        if match:
            value = {'y': '1', 'n': '0'}.get(match[2], match[2])
            if value != '0':
                result[match[1]] = value
    if header:
        # IDF's backward-compatibility names are macro aliases in sdkconfig.h,
        # while sdkconfig serializes their resolved values.
        for key in result:
            value, seen = result[key], {key}
            while re.fullmatch(r'CONFIG_\w+', value):
                if value in seen:
                    raise ValueError(f'Cyclic sdkconfig.h alias: {key}')
                seen.add(value)
                value = result.get(value, '0')
            result[key] = value
        result = {k: v for k, v in result.items() if v != '0'}
    return result


def canonical(value):
    if re.fullmatch(r'(?:0x[0-9a-fA-F]+|[0-9]+)', value):
        return str(int(value, 16 if value.startswith('0x') else 10))
    return value


def differences(reference, actual):
    return {key: {'reference': reference.get(key, '0'), 'actual': actual.get(key, '0')}
            for key in sorted(reference.keys() | actual.keys())
            if canonical(reference.get(key, '0')) != canonical(actual.get(key, '0'))}


def validate(environment, actual, reference):
    cpu, sleep, _ = PROFILES[environment]
    expected = {
        'CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ': str(cpu),
        'CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ': str(cpu),
        'CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160': str(int(cpu == 160)),
        'CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240': str(int(cpu == 240)),
        'CONFIG_ESP32_DEFAULT_CPU_FREQ_160': str(int(cpu == 160)),
        'CONFIG_ESP32_DEFAULT_CPU_FREQ_240': str(int(cpu == 240)),
        **{key: str(int(sleep)) for key in SLEEP_KEYS},
    }
    for key, value in expected.items():
        if actual.get(key, '0') != value:
            raise ValueError(f'{environment}: stale/wrong {key}={actual.get(key, "0")}; expected {value}')
    changes = differences(reference, actual)
    extra = set(changes) - ALLOWED_KEYS
    if extra:
        raise ValueError(f'{environment}: unrelated sdkconfig differences: {sorted(extra)}')
    return changes


def baseline(project):
    reference = json.loads((Path(project) / 'docs/matrix-reference.json').read_text())
    values = reference['official_baseline_sdkconfig']
    encoded = json.dumps(values, sort_keys=True, separators=(',', ':')).encode()
    if hashlib.sha256(encoded).hexdigest() != reference['official_baseline_sdkconfig_sha256']:
        raise ValueError('Matrix baseline manifest is inconsistent')
    return values
