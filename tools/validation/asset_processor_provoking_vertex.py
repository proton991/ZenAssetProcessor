"""Run the standalone indexed flat-shading acceptance probe on named Vulkan devices."""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

PROCESSOR_ROOT = Path(__file__).resolve().parents[2]


def digest(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def read_table(path: Path) -> list[dict[str, str]]:
    with path.open(encoding='utf-8') as source:
        return list(csv.DictReader(source, delimiter='\t'))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True, help='Configured/built provoking_vertex evaluation directory')
    parser.add_argument('--output', type=Path, required=True, help='New evidence directory inside processor repository')
    parser.add_argument('--device', action='append', required=True, help='Vulkan device name substring; repeat for multiple devices')
    parser.add_argument('--executable-alias', help='Optional executable basename for local overlay exclusion, e.g. 7zFM.exe')
    args = parser.parse_args()
    build = args.build.resolve()
    output = args.output.resolve()
    if not output.is_relative_to(PROCESSOR_ROOT.resolve()):
        parser.error('--output must be inside the processor repository')
    if output.exists():
        parser.error('--output must not exist; preserve earlier evidence')
    executable = build / ('provoking_vertex.exe' if os.name == 'nt' else 'provoking_vertex')
    fixture_root = PROCESSOR_ROOT / 'tests/integration/zenengine/Editor/Fixtures'
    inputs = [executable, build / 'provoking.vert.spv', build / 'provoking.frag.spv',
              fixture_root / 'provoking_scene.gltf', fixture_root / 'provoking_rotated.gltf']
    for path in inputs:
        if not path.is_file():
            parser.error(f'missing required input: {path}')
    if args.executable_alias and Path(args.executable_alias).name != args.executable_alias:
        parser.error('--executable-alias must be a basename')
    output.mkdir(parents=True)
    if args.executable_alias:
        executable = output / args.executable_alias
        shutil.copy2(inputs[0], executable)
    sources = list((PROCESSOR_ROOT / 'tools/evaluation/provoking_vertex').glob('*'))
    sources += [fixture_root / 'generate_provoking_scene.py', Path(__file__).resolve()]
    evidence = {'inputs': {str(path): digest(path) for path in inputs + sources if path.is_file()},
                'invocation': sys.argv, 'devices': [], 'passed': True}
    for index, device in enumerate(args.device):
        run = output / f'device-{index}'
        environment = dict(os.environ)
        settings = {'ZEN_VULKAN_DEVICE': device, 'VK_LAYER_VALIDATE_SYNC': '1',
                    'VK_LOADER_LAYERS_DISABLE': '~implicit~', 'DISABLE_RTSS_LAYER': '1'}
        environment.update(settings)
        command = [str(executable), str(inputs[3]), str(inputs[4]), str(inputs[1]), str(inputs[2]), str(run)]
        log = output / f'device-{index}.log'
        with log.open('w', encoding='utf-8') as stream:
            completed = subprocess.run(command, env=environment, cwd=PROCESSOR_ROOT,
                                       stdout=stream, stderr=subprocess.STDOUT, timeout=120, check=False)
        result = {'requested_device': device, 'command': command, 'environment': settings,
                  'exit_code': completed.returncode, 'log': str(log), 'passed': completed.returncode == 0}
        if completed.returncode == 0:
            result['device'] = dict(line.split('=', 1) for line in (run / 'device.txt').read_text().splitlines())
            result['comparisons'] = read_table(run / 'comparisons.tsv')
            result['packages'] = read_table(run / 'packages.tsv')
            result['passed'] &= result['device']['passed'] == 'true' and result['device']['validation_errors'] == '0'
            expected_modes = 2 if result['device']['last_vertex'] == 'tested' else 1
            result['passed'] &= len(result['comparisons']) == 7 * expected_modes
            for comparison in result['comparisons']:
                rotated = comparison['case'].startswith('rotated-')
                expected = 'rotated_different_channels' if rotated else 'reference_different_channels'
                control = 'reference_different_channels' if rotated else 'rotated_different_channels'
                result['passed'] &= comparison[expected] == '0' and int(comparison[control]) > 1000 and comparison['oracle_failures'] == '0'
            # Also inspect cleanup messages emitted after the in-process error count was recorded.
            log_text = log.read_text(encoding='utf-8')
            result['passed'] &= not any(marker in log_text for marker in ('Validation Error', 'VUID-', 'cleanup wait failed', 'recovery wait failed'))
            result['artifacts'] = {str(path.relative_to(output)): digest(path) for path in sorted(run.iterdir()) if path.is_file()}
        evidence['devices'].append(result)
        evidence['passed'] &= result['passed']
        print(f'{device}: {"PASS" if result["passed"] else "FAIL"}', flush=True)
    (output / 'results.json').write_text(json.dumps(evidence, indent=2) + '\n', encoding='utf-8')
    return 0 if evidence['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
