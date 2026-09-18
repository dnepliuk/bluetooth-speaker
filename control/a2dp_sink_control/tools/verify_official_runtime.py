"""Run production official service with Windows thread/API test adapters.

No downloads, firmware builds, serial access or SDK modifications.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess


def check_runtime(zig):
    project = Path(__file__).resolve().parents[1]
    work = project / '.pio/official-host'
    include = work / 'include'
    for name in ('freertos/FreeRTOS.h', 'freertos/task.h', 'freertos/semphr.h',
                 'freertos/ringbuf.h', 'driver/i2s_std.h', 'esp_log.h',
                 'esp_timer.h', 'esp_clk_tree.h', 'esp_a2dp_api.h', 'esp_err.h'):
        stub = include / name
        stub.parent.mkdir(parents=True, exist_ok=True)
        stub.write_text('#include "official_host_stubs.h"\n')
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(work / 'zig-cache'),
               ZIG_LOCAL_CACHE_DIR=str(work / 'zig-local-cache'))
    exe = work / 'test_official.exe'
    subprocess.run([str(Path(zig).resolve()), 'cc', '-std=c11', '-O2', '-UNDEBUG',
                    '-Wall', '-Wextra', '-Werror', '-DCONTROL_VARIANT=9',
                    '-DCONTROL_CMAKE_VARIANT=9',
                    '-DCONTROL_OFFICIAL_ENV_NAME="control-official-i2s-reference"',
                    '-I' + str(include),
                    '-I' + str(project / 'tools'), str(project / 'tools/test_official.c'),
                    '-o', str(exe)], env=env, check=True, timeout=180)
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr
    return {'checks': 'PASS', 'result': result.stdout.strip(),
            'target': 'Production control_official_i2s.c, Windows threads, API adapters',
            'limits': 'Does not validate real FreeRTOS RingBuffer/scheduling, Bluetooth, DMA or board behavior.'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig', required=True)
    args = parser.parse_args()
    print(json.dumps(check_runtime(args.zig), indent=2))
