"""PlatformIO pre/post guard. Read-only; never patches or deletes sdkconfig.

Pre catches stale generated configs before Kconfig runs; post also checks a new
generated config/header. Even incremental builds run the guard. Repair an
intentional mismatch in only the named environment's configuration.
"""
from pathlib import Path
import sys

Import('env')
from SCons.Script import COMMAND_LINE_TARGETS

project = Path(env.subst('$PROJECT_DIR')).resolve()
sys.path.insert(0, str(project / 'tools'))
from official_matrix import PROFILES, baseline, settings, validate

def check_current_environment():
    name = env.subst('$PIOENV')
    if name not in PROFILES:
        raise ValueError('Matrix guard used outside the four official environments')
    requested = env.BoardConfig().get('build.esp-idf.sdkconfig_path')
    config = (project / requested).resolve()
    expected = project / ('sdkconfig.' + name)
    if config != expected:
        raise ValueError(f'{name}: sdkconfig path must be {expected}')
    build = Path(env.subst('$BUILD_DIR')).resolve()
    if build.name != name:
        raise ValueError(f'{name}: shared/mismatched build directory: {build}')
    original = baseline(project)
    if config.is_file():
        validate(name, settings(config), original)
    header = build / 'config/sdkconfig.h'
    if header.is_file():
        validate(name, settings(header, header=True), original)


# Permit the standard repair tools even when a stale configuration is detected.
if not env.GetOption('clean') and 'menuconfig' not in COMMAND_LINE_TARGETS:
    try:
        check_current_environment()
    except (ValueError, TypeError, KeyError) as error:
        print(f'CPU/sleep matrix guard: {error}\n'
              'Build stopped; the guard did not modify configuration. Correct only this '
              'environment via its sdkconfig/Menuconfig (Clean if the generated header is stale); '
              'defaults do not override an existing sdkconfig.')
        env.Exit(1)
