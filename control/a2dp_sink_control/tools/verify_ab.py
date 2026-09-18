"""Read-only checks of already-built A/B firmware; never builds or uploads.

Run with PlatformIO's Python (it supplies pyelftools). Results go to stdout.
"""

import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

from elftools.elf.elffile import ELFFile
from verify_clocked import check_clocked
from verify_spsc import check_spsc
from verify_aligned import check_aligned
from verify_official import check_official
from verify_matrix import check_matrix

PROJECT = Path(__file__).resolve().parents[1]
ROOT = PROJECT.parents[1]
VARIANTS = ("baseline", "main-config", "streambuffer", "i2s-silence", "pipeline", "clocked-pipeline", "spsc-pipeline", "spsc-dma-aligned", "official-i2s-reference")


def sha(data):
    return hashlib.sha256(data).hexdigest()


def config(path):
    result = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        enabled = re.fullmatch(r"(CONFIG_\w+)=(.*)", line)
        disabled = re.fullmatch(r"# (CONFIG_\w+) is not set", line)
        if enabled:
            result[enabled[1]] = enabled[2]
        elif disabled:
            result[disabled[1]] = "n"
    return result


def semantic_digest(values):
    # Disabled and absent symbols are equivalent for this build comparison.
    return sha(json.dumps({k: v for k, v in values.items() if v != "n"},
                          sort_keys=True, separators=(",", ":")).encode())


def function(text, name):
    match = re.search(r"static void " + name + r"\([^)]*\)\s*\{", text)
    assert match, name
    start = match.start()
    pos = match.end()
    depth = 1
    while depth:
        depth += (text[pos] == "{") - (text[pos] == "}")
        pos += 1
    return text[start:pos].replace("\r\n", "\n")


def check():
    reference = json.loads((PROJECT / "docs/reference.json").read_text(encoding="utf-8"))
    assert sha("".join(row["path"] + "\t" + row["sha256"] + "\n"
                       for row in reference["protected_files"]).encode()) == reference["protected_manifest_sha256"]
    main = {row["path"]: sha((ROOT / row["path"]).read_bytes())
            for row in reference["protected_files"]}
    for row in reference["protected_files"]:
        assert main[row["path"]] == row["sha256"], "Main file changed: " + row["path"]
    source = (PROJECT / "src/main.c").read_text(encoding="utf-8")
    callback_hash = sha(function(source, "pcm_data_callback").encode())
    assert callback_hash == reference["baseline_callback_sha256"], "Baseline callback changed"
    assert sha((PROJECT / "sdkconfig.defaults").read_bytes()) == reference["baseline_defaults_sha256"]

    baseline = config(PROJECT / "sdkconfig.control-baseline")
    assert semantic_digest(baseline) == reference["baseline_sdkconfig_semantic_sha256"], \
        "Generated baseline config differs from the working reference"
    results = []
    for variant in VARIANTS:
        env = "control-" + variant
        values = config(PROJECT / ("sdkconfig." + env))
        if variant in ("spsc-pipeline", "spsc-dma-aligned", "official-i2s-reference"):
            assert semantic_digest(values) == semantic_digest(config(
                PROJECT / "sdkconfig.control-clocked-pipeline")), \
                'SPSC and clocked must have identical complete sdkconfig semantics'
        expected = dict(reference["relevant_baseline_settings"])
        if variant == "main-config":
            expected.update(reference["relevant_main_changes"])
        for key, value in expected.items():
            assert values.get(key, "n") == value, (env, key, value, values.get(key))
        # BOYA and partition layout are deliberately not part of the config A/B.
        for key in ("CONFIG_SPI_FLASH_SUPPORT_BOYA_CHIP", "CONFIG_PARTITION_TABLE_FILENAME"):
            assert values.get(key, "n") == baseline.get(key, "n"), (env, key)
        build = PROJECT / ".pio/build" / env
        description = json.loads((build / "project_description.json").read_text())
        assert Path(description["config_file"]).resolve() == (PROJECT / ("sdkconfig." + env)).resolve()
        elf_path = build / "firmware.elf"
        with elf_path.open("rb") as handle:
            elf = ELFFile(handle)
            symbols = {symbol.name for symbol in elf.get_section_by_name(".symtab").iter_symbols()
                       if symbol["st_shndx"] != "SHN_UNDEF"}
            rodata = b"".join(section.data() for section in elf.iter_sections()
                              if section["sh_type"] == "SHT_PROGBITS" and "rodata" in section.name)
        assert variant.encode() + b"\0" in rodata, (env, "variant literal missing")
        assert b"CONTROL variant=%s" in rodata, (env, "banner missing")
        has_stream = variant in ("streambuffer", "pipeline", "clocked-pipeline")
        has_spsc = variant in ("spsc-pipeline", "spsc-dma-aligned")
        has_i2s = variant in ("i2s-silence", "pipeline", "clocked-pipeline", "spsc-pipeline", "spsc-dma-aligned")
        official = variant == "official-i2s-reference"
        component_names = {Path(path).name for path in description["build_component_paths"]}
        assert ("esp_driver_i2s" in component_names) == (has_i2s or official), env
        commands = json.loads((build / "compile_commands.json").read_text())
        app_sources = {Path(entry["file"]).name for entry in commands
                       if Path(entry["file"]).parent.resolve() == (PROJECT / "src").resolve()}
        expected_sources = {"main.c"}
        if has_stream: expected_sources.add("control_stream.c")
        if has_spsc: expected_sources.add("control_spsc.c")
        if has_stream or has_i2s: expected_sources.add("control_transport.c")
        if has_i2s: expected_sources.add("control_i2s.c")
        if official: expected_sources = {"control_official_main.c", "control_official_i2s.c"}
        assert app_sources == expected_sources, (env, app_sources)
        assert not any(Path(entry["file"]).parent.resolve() == (ROOT / "src").resolve()
                       for entry in commands), "Build depends on main sources"
        assert ("buffered_pcm_data_callback" in symbols) == (has_stream or has_spsc), env
        assert ("control_stream_send" in symbols) == has_stream, env
        assert ("control_spsc_send" in symbols) == has_spsc, env
        if not has_spsc:
            assert not any(name.startswith("control_spsc_") for name in symbols), env
        assert ("control_i2s_write" in symbols) == has_i2s, env
        assert ("clocked_loop" in symbols) == (variant in ("clocked-pipeline", "spsc-pipeline", "spsc-dma-aligned")), env
        assert not ({"esp_avrc_ct_init", "esp_avrc_tg_init"} & symbols), env
        if not has_stream:
            assert not any(name.startswith("xStreamBuffer") for name in symbols), env
        if not has_i2s and not official:
            assert "i2s_channel_write" not in symbols, env
        if variant in ("baseline", "main-config"):
            assert "control_transport_init" not in symbols, env
        results.append({"environment": env, "checks": "PASS", "banner": "CONTROL variant=" + variant,
                        "firmware_sha256": sha((build / "firmware.bin").read_bytes()),
                        "sdkconfig_semantic_sha256": semantic_digest(values)})
    # Test the actual selector header, including disagreeing build systems.
    compiler = Path.home() / ".platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32-elf-gcc.exe"
    header = PROJECT / "src/control_variant.h"
    cases = [(v, v, True) for v in range(1, 10)] + [
        (0, 0, False), (10, 10, False), (1, 3, False), (6, 5, False),
        (5, 6, False), (7, 6, False), (6, 7, False), (8, 7, False), (7, 8, False),
        (9, 8, False), (8, 9, False), ("8,9", 9, False), ("8|9", 9, False),
        ("8+1", 9, False), (9, "8,9", False)]
    for c_variant, cmake_variant, valid in cases:
        test = subprocess.run([str(compiler), "-x", "c", "-fsyntax-only", "-include", str(header),
                               f"-DCONTROL_VARIANT={c_variant}", f"-DCONTROL_CMAKE_VARIANT={cmake_variant}", "-"],
                              input="", text=True, capture_output=True)
        assert (test.returncode == 0) == valid, test.stderr
    missing = subprocess.run([str(compiler), "-x", "c", "-fsyntax-only", "-include", str(header), "-"],
                             input="", text=True, capture_output=True)
    assert missing.returncode != 0, "Missing variant was accepted"
    return {"protected_files": len(main), "main_unchanged": True,
            "protected_manifest_sha256": reference["protected_manifest_sha256"],
            "baseline_callback_sha256": callback_hash,
            "baseline_config_matches_working_reference": True,
            "clocked_and_legacy_regression_checks": check_clocked(PROJECT),
            "spsc_and_six_variant_regression_checks": check_spsc(PROJECT),
            "aligned_and_seven_variant_regression_checks": check_aligned(PROJECT),
            "official_and_eight_variant_regression_checks": check_official(PROJECT),
            "official_cpu_sleep_matrix_checks": check_matrix(PROJECT),
            "selector_positive_and_negative_checks": "PASS", "environments": results,
            "hardware_tests": "Historical user reports: baseline/main-config/streambuffer/i2s-silence PASS; pipeline/clocked/SPSC/aligned/official-reference FAIL. CPU/sleep matrix hardware NOT RUN. No Upload or Monitor by the agent."}


if __name__ == "__main__":
    result = check()
    json.dump(result, sys.stdout, indent=2)
    print()
    sys.exit(0 if all(result[key]['checks'] == 'PASS' for key in (
        'spsc_and_six_variant_regression_checks', 'aligned_and_seven_variant_regression_checks',
        'official_and_eight_variant_regression_checks', 'official_cpu_sleep_matrix_checks')) else 1)
