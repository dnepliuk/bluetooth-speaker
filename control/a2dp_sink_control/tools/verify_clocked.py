"""Compiler-preprocessed regression evidence for the five existing variants."""

import hashlib
import json
from pathlib import Path
import re
import subprocess


def preprocess(project, variant, filename):
    build = project / '.pio/build' / ('control-' + variant)
    commands = json.loads((build / 'compile_commands.json').read_text())
    entry = next(row for row in commands if Path(row['file']).name == filename
                 and Path(row['file']).parent.resolve() == (project / 'src').resolve())
    command = re.sub(r' -o .*? -c ', ' -E -P ', entry['command'])
    # PlatformIO adds build_flags during SCons, outside CMake's command database.
    selector = re.search(r'-DCONTROL_CMAKE_VARIANT=(\d+)', command)[1]
    command += ' -DCONTROL_VARIANT=' + selector
    result = subprocess.run(command, cwd=entry['directory'], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return result.stdout


def digest(text):
    return hashlib.sha256(text.encode()).hexdigest()


def legacy_sources(project):
    result = {}
    for variant in ('baseline', 'main-config', 'streambuffer', 'i2s-silence', 'pipeline'):
        names = ['main.c']
        if variant in ('streambuffer', 'pipeline'): names.append('control_stream.c')
        if variant in ('streambuffer', 'i2s-silence', 'pipeline'): names.append('control_transport.c')
        if variant in ('i2s-silence', 'pipeline'): names.append('control_i2s.c')
        result[variant] = {name: digest(preprocess(project, variant, name)) for name in names}
    return result


def body(source, name):
    match = re.search(r'(?:static )?(?:void|size_t|esp_err_t) ' + name + r'\([^)]*\)\s*\{', source)
    assert match, name
    pos, depth = match.end(), 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[match.start():pos]


def check_clocked(project):
    reference = json.loads((project / 'docs/clocked-reference.json').read_text())
    assert legacy_sources(project) == reference['legacy_preprocessed_sha256'], \
        'An existing variant changed after compiler preprocessing'
    for name, expected in reference['unchanged_source_sha256'].items():
        assert digest((project / name).read_text(encoding='utf-8')) == expected, name
    clocked = preprocess(project, 'clocked-pipeline', 'control_transport.c')
    legacy = preprocess(project, 'pipeline', 'control_transport.c')
    for name in ('control_transport_send', 'control_transport_callback_done'):
        assert body(clocked, name) == body(legacy, name), 'Producer changed: ' + name
    # Check active compiler output, not just code inside potentially unused #ifs.
    read = body(clocked, 'clocked_read')
    assert 'control_stream_receive(data, size, 0)' in read
    assert len(re.findall(r'control_stream_receive\(', clocked)) == 2, \
        'Expected only the declaration and the zero-timeout call'
    loop = body(clocked, 'clocked_loop')
    assert not any(word in loop for word in ('vTaskDelay', 'portMAX_DELAY', 'xStreamBufferReset'))
    assert 'clocked_write(chunk, pcm_size, epoch)' in loop
    assert 'write_all' not in clocked
    write = body(clocked, 'clocked_write')
    assert 'control_i2s_write(chunk + offset, remaining, &written)' in write
    assert len(re.findall(r'vTaskDelay\(', write)) == 1
    assert 'if (written == 0) vTaskDelay(1)' in write
    stream = preprocess(project, 'clocked-pipeline', 'control_stream.c')
    assert 'xStreamBufferReset' not in body(stream, 'control_stream_receive')
    assert 'xStreamBufferReceive(s_stream, data, size, timeout)' in stream
    i2s = preprocess(project, 'clocked-pipeline', 'control_i2s.c')
    assert 'i2s_channel_write(s_tx, data, size, written, 1000U)' in i2s
    return {
        'legacy_application_translation_units_unchanged': sum(
            len(files) for files in reference['legacy_preprocessed_sha256'].values()),
        'pipeline_producer_identical': True,
        'clocked_receive_timeout': 0,
        'steady_state_software_delay': False,
        'i2s_write_timeout_ms': 1000,
        'checks': 'PASS',
    }
