"""Verify and render the saved, original M4 skin/morph/order fixtures."""
import argparse
import hashlib
import json
from pathlib import Path

from asset_processor_array_report import read_ppm, write_png


def differences(left, right):
    assert len(left) == len(right)
    errors = [abs(a-b) for a, b in zip(left, right)]
    return {'different_channels': sum(value != 0 for value in errors),
            'maximum_channel_error': max(errors)}


def report(directory):
    images = {}
    for mode in ('inline', 'threaded'):
        for package in ('reference', 'optimized', 'reversed'):
            assert (directory / f'geometry-{mode}-{package}.zenasset').is_file()
            for clip in range(3):
                for pose in range(5):
                    name = f'geometry-{mode}-{package}-c{clip}-p{pose}'
                    path = directory / f'{name}.ppm'
                    width, height, pixels = read_ppm(path)
                    assert (width, height) == (256, 192)
                    colored = sum(max(pixels[i:i+3])-min(pixels[i:i+3]) > 8
                                  for i in range(0, len(pixels), 3))
                    assert colored > 256, f'Blank/neutral capture: {path}'
                    images[mode, package, clip, pose] = pixels
                    write_png(directory / f'{name}.png', width, height, pixels)
    assert len(list(directory.glob('*.ppm'))) == 90
    pairs, order_controls, pose_controls, execution_pairs = [], [], [], []
    for mode in ('inline', 'threaded'):
        for clip in range(3):
            for pose in range(5):
                original = images[mode, 'reference', clip, pose]
                pairs.append({'mode': mode, 'clip': clip, 'pose_seconds': pose*.5,
                              **differences(original, images[mode, 'optimized', clip, pose])})
                order_controls.append({'mode': mode, 'clip': clip, 'pose_seconds': pose*.5,
                                       **differences(original, images[mode, 'reversed', clip, pose])})
            for package in ('reference', 'optimized', 'reversed'):
                pose_controls.append({'mode': mode, 'clip': clip, 'package': package,
                                      **differences(images[mode, package, clip, 0], images[mode, package, clip, 4])})
    for package in ('reference', 'optimized', 'reversed'):
        for clip in range(3):
            for pose in range(5):
                execution_pairs.append({'package': package, 'clip': clip, 'pose_seconds': pose*.5,
                                        **differences(images['inline', package, clip, pose],
                                                      images['threaded', package, clip, pose])})
    assert all(row['different_channels'] == 0 for row in pairs + execution_pairs)
    assert all(row['different_channels'] > 0 for row in order_controls + pose_controls)
    files = [{'name': path.name, 'bytes': path.stat().st_size,
              'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
             for path in sorted(directory.iterdir()) if path.suffix in ('.ppm', '.png', '.zenasset')]
    result = {'passed': True, 'scope': 'One original transparent fixture, three clips, five poses, two execution modes. RGB captures only.',
              'reference_vs_optimized': pairs, 'reversed_triangle_controls': order_controls,
              'first_vs_last_pose_controls': pose_controls, 'inline_vs_threaded': execution_pairs, 'files': files}
    (directory / 'results.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(f'{len(pairs)} exact cooked/reference pairs; {len(order_controls)} visible triangle-order controls; '
          f'{len(pose_controls)} visible pose controls; {len(execution_pairs)} exact execution-mode pairs.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('directory', type=Path)
    report(parser.parse_args().directory)
