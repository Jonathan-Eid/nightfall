"""Bounded GPU scene wrapper for a host-registered physical-headset XR profile.

Register the Python executable as the profile application, with this script and
--build <native build directory> as arguments. Meteor supplies the per-session
runtime, tracking and transport environment. This script changes no registration,
firewall, pairings or headset settings. It is not a standalone streaming host.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True, help='Parent of probe/Release/')
    parser.add_argument('--frames', type=int, default=3600)
    parser.add_argument('--timeout', type=int, default=180, help='Maximum trial duration in seconds')
    args = parser.parse_args()
    if not 1 <= args.frames <= 7200 or not 1 <= args.timeout <= 600:
        parser.error('frames must be 1..7200 and timeout must be 1..600')
    build = args.build.resolve()
    probe = build / 'probe/Release/meteor-runtime-probe.exe'
    shader = build / 'probe/meteor_gpu_scene.spv'
    if not probe.is_file() or not shader.is_file():
        parser.error('Build directory must contain the native runtime probe and GPU scene shader')
    env = os.environ.copy()
    for key in ['METEOR_XR_QUAD_FIXTURE', 'METEOR_XR_PANORAMA_FIXTURE',
                'METEOR_XR_CYLINDER_FIXTURE', 'METEOR_XR_STACK_FIXTURE',
                'METEOR_XR_LAYER_MATRIX', 'METEOR_XR_GPU_RASTER_SCENE']:
        env.pop(key, None)
    env.update({
        'METEOR_CONTINUOUS_FRAMES': str(args.frames),
        'METEOR_XR_FREE_RUNNING': '1',
        'METEOR_XR_TRACKED_SCENE': '1',
        'METEOR_XR_GPU_SCENE_SHADER': str(shader),
        # Without measurement mode, every frame generates/validates a CPU
        # reference and appends raw eye images to disk. That is not an FPS test.
        'METEOR_XR_GPU_MEASURE': '1',
        'METEOR_XR_OBSERVE_INPUT': '1',
        'METEOR_OVR_CAPTURE_ACK': '1',
    })
    try:
        return subprocess.run([str(probe)], env=env, stdin=subprocess.DEVNULL,
                              timeout=args.timeout).returncode
    except subprocess.TimeoutExpired:
        print('Headset GPU trial exceeded its time limit', file=sys.stderr)
        return 124


if __name__ == '__main__':
    raise SystemExit(main())
