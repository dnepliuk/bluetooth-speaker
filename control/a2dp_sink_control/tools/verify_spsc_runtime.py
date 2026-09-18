"""Compile/run production ring on Windows using a supplied standalone Zig compiler.

Example: python tools/verify_spsc_runtime.py --zig .pio/host-tools/ziglang/zig.exe
No downloads, build of firmware, Upload or Monitor. Host stubs only replace IDF
placement, error typedefs and timer; the ring/atomics/memcpy are production C.
"""

import argparse
import json
import os
from pathlib import Path
import subprocess


def check_runtime(zig, variant=7):
    assert variant in (7, 8), 'Host tests cover the two SPSC variants'
    project = Path(__file__).resolve().parents[1]
    work = project / '.pio/spsc-host'
    include = work / 'include'
    include.mkdir(parents=True, exist_ok=True)
    stubs = {
        'esp_err.h': '#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n',
        'esp_attr.h': '#pragma once\n#define DRAM_ATTR\n',
        'esp_timer.h': '#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n',
    }
    for name, content in stubs.items():
        (include / name).write_text(content)
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(work / 'zig-cache'),
               ZIG_LOCAL_CACHE_DIR=str(work / 'zig-local-cache'))
    exe = work / f'test_spsc_v{variant}.exe'
    command = [str(Path(zig).resolve()), 'cc', '-std=c11', '-O2', '-UNDEBUG', '-Wall', '-Wextra',
               '-Werror', f'-DCONTROL_VARIANT={variant}', f'-DCONTROL_CMAKE_VARIANT={variant}',
               '-I' + str(include), str(project / 'tools/test_spsc.c'), '-o', str(exe)]
    subprocess.run(command, env=env, check=True, timeout=180)
    result = subprocess.run([str(exe)], capture_output=True, text=True, check=True, timeout=60)
    return {'checks': 'PASS', 'variant': variant, 'output_bytes': 3840 if variant == 8 else 4096,
        'compiler': subprocess.check_output(
        [str(Path(zig).resolve()), 'version'], text=True).strip(),
        'target': 'Windows host; production C ring, IDF timer/placement stubs',
        'result': result.stdout.strip(),
        'limits': 'Does not execute Bluetooth, I2S, ESP32 memory ordering or scheduler.'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig', required=True)
    parser.add_argument('--variant', type=int, choices=(7, 8),
                        help='Default: run both 4096-byte and 3840-byte variants')
    args = parser.parse_args()
    variants = (args.variant,) if args.variant else (7, 8)
    print(json.dumps({'checks': 'PASS', 'variants': [check_runtime(args.zig, v) for v in variants]}, indent=2))
