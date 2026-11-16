"""Headset-free OpenXR -> Nightfall/QSV -> production wire-validator integration check.

Uses isolated child-process environment; never registers a runtime or changes services.
The tracking publisher is a synthetic fixture, not evidence of physical headset tracking.
"""
import argparse
import json
import math
import os
from pathlib import Path
import socket
import subprocess
import sys
import time


def port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True, help='Standalone VDXR build directory')
    parser.add_argument('--build', type=Path, required=True, help='Parent of probe/, ovr/, wire-sink.exe')
    parser.add_argument('--output', type=Path, required=True, help='New artifact directory')
    parser.add_argument('--codec', choices=['h264', 'h265'], default='h264')
    parser.add_argument('--frames', type=int, default=24)
    parser.add_argument('--hz', type=int, choices=[60,72,90], default=72)
    parser.add_argument('--fov-scale', type=float, default=1.0, help='Tangent-space render margin (1..1.4)')
    parser.add_argument('--quad', action='store_true')
    parser.add_argument('--frame-ids', action='store_true', help='Verify a changing per-frame color in both decoded eyes')
    parser.add_argument('--gpu', action='store_true', help='Use the GPU performance probe wrapper')
    parser.add_argument('--preflight', action='store_true', help='Destroy a device-detection instance before the actual VR instance')
    args = parser.parse_args()
    if not math.isfinite(args.fov_scale) or not 1 <= args.fov_scale <= 1.4:
        parser.error('fov-scale must be 1..1.4')
    if args.quad and args.gpu:
        parser.error('--quad pixel-reference checks and --gpu measurement are separate fixtures')
    if args.frame_ids and (args.quad or args.gpu):
        parser.error('--frame-ids requires the plain stereo pattern')
    if not 1 <= args.frames <= 240:
        parser.error('frames must be 1..240')
    build, runtime, out = args.build.resolve(), args.runtime.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    tracking_port, video_port = port(), port()
    while tracking_port == video_port:
        video_port = port()
    env = os.environ.copy()
    # Clear fixture flags that might otherwise change the test's composition contract.
    for key in ['METEOR_XR_QUAD_FIXTURE', 'METEOR_XR_PANORAMA_FIXTURE', 'METEOR_XR_CYLINDER_FIXTURE',
                'METEOR_XR_STACK_FIXTURE', 'METEOR_XR_GPU_SCENE_SHADER', 'METEOR_XR_GPU_MEASURE',
                'METEOR_PROBE_HOLD_MS', 'METEOR_XR_PREFLIGHT']:
        env.pop(key, None)
    env.update({
        'XR_RUNTIME_JSON': str(runtime / 'virtualdesktop-openxr.json'),
        'NIGHTFALL_OVR_BRIDGE': str(build / 'ovr/Release/meteor-ovr-bridge.dll'),
        'METEOR_WINDOWS_ENCODER': str(build / 'ovr/Release/meteor-ovr-bridge.dll'),
        'METEOR_TRACKING_BRIDGE': str(build / 'ovr/Release/meteor-windows-tracking.dll'),
        'METEOR_TRACKING_PORT': str(tracking_port), 'METEOR_XR_DIRECT_PORT': str(video_port),
        'METEOR_XR_CONTROLLERS': '1', 'METEOR_XR_TOUCH_INPUTS': '1', 'METEOR_XR_HAPTICS': '1',
        'METEOR_XR_HEADSET_OPTICS': '1', 'METEOR_XR_TRACKED_SCENE': '1',
        'METEOR_XR_FOV_SCALE': str(args.fov_scale),
        'METEOR_XR_EYE_SIZE': '1280', 'METEOR_XR_EYE_HEIGHT': '1344',
        'XRT_COMPOSITOR_NULL_FPS': str(args.hz), 'METEOR_XR_CODEC': args.codec, 'METEOR_XR_BITRATE_MBPS': '32',
        'METEOR_PROBE_OUTPUT': str(out), 'METEOR_OVR_CAPTURE_ACK': '1',
        'METEOR_CONTINUOUS_FRAMES': str(args.frames), 'METEOR_XR_FREE_RUNNING': '1',
    })
    if args.quad:
        env['METEOR_XR_QUAD_FIXTURE'] = '1'
    if args.preflight:
        env['METEOR_XR_PREFLIGHT'] = '1'
    if args.frame_ids:
        env.pop('METEOR_XR_TRACKED_SCENE', None)
    children, files = [], []
    result = {'success': False, 'synthetic_tracking': True, 'gpu_measurement': args.gpu, 'codec': args.codec, 'target_hz':args.hz,
              'dimensions': [2560, 1344], 'requested_frames': args.frames, 'quad': args.quad,
              'preflight': args.preflight, 'frame_ids': args.frame_ids}
    result['fov_scale'] = args.fov_scale

    def start(name, command):
        stdout, stderr = out / (name + '.out'), out / (name + '.err')
        handles = [stdout.open('wb'), stderr.open('wb')]
        files.extend(handles)
        child = subprocess.Popen(list(map(str, command)), env=env, stdin=subprocess.DEVNULL,
                                 stdout=handles[0], stderr=handles[1],
                                 creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        children.append(child)
        return child

    def ready(child, name, text):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if text in (out / (name + '.out')).read_text(errors='replace'):
                return
            if child.poll() is not None:
                raise RuntimeError(name + ' exited before readiness')
            time.sleep(.05)
        raise RuntimeError(name + ' readiness timeout')

    try:
        publisher = start('tracking', [build / 'probe/Release/meteor-tracking-socket.exe', '--serve', tracking_port])
        ready(publisher, 'tracking', 'Synthetic tracking fixture listening')
        sink = start('sink', [build / 'wire-sink.exe', video_port, out / ('stream.' + args.codec), '.5'])
        ready(sink, 'sink', 'wire_sink_port=')
        command = [build / 'probe/Release/meteor-runtime-probe.exe']
        if args.gpu:
            command = [sys.executable, Path(__file__).with_name('windows_vdxr_gpu_probe.py'),
                       '--build', build, '--frames', str(args.frames), '--timeout', '60']
        probe = start('probe', command)
        result['probe_exit'] = probe.wait(timeout=60)
        if result['probe_exit']:
            raise RuntimeError('OpenXR probe failed; see probe.err')
        result['sink_exit'] = sink.wait(timeout=10)
        if result['sink_exit']:
            raise RuntimeError('Production wire validator rejected the stream; see sink.err')
        wire = (out / 'sink.out').read_text()
        result['validated_frames'] = wire.count('validated_frame=')
        result['clean_end'] = 'frames and clean END through Meteor wire validator' in wire
        result['stream_rate_verified'] = wire.count('fps=' + str(args.hz) + ' ') == args.frames
        if result['validated_frames'] != args.frames or not result['clean_end'] or not result['stream_rate_verified']:
            raise RuntimeError('Incomplete frame delivery or missing END acknowledgement')
        expected_center = 'Tracked center=0.250000,-1.175000,-0.750000' if args.frame_ids else 'Tracked center=0.250000,0.500000,-0.750000'
        if expected_center not in (out / 'probe.out').read_text():
            raise RuntimeError('OpenXR tracking-space conversion did not preserve the fixture floor pose')
        if args.frame_ids:
            # A static scene cannot reveal an old swapchain image being paired
            # with the current frame's pose. Check every changing image in both
            # eyes, including startup and wraparound of the three-image ring.
            import av

            def srgb(value):
                linear = value / 255
                return round(255 * (12.92 * linear if linear <= .0031308 else 1.055 * linear ** (1 / 2.4) - .055))

            measurements = []
            with av.open(str(out / ('stream.' + args.codec))) as capture:
                for frame_id, frame in enumerate(capture.decode(video=0)):
                    rgb = frame.reformat(format='rgb24')
                    plane = rgb.planes[0]
                    pixels = bytes(plane)
                    eyes = []
                    for eye in range(2):
                        x, y = eye * 1280 + 320, 336
                        start = y * plane.line_size + x * 3
                        eyes.append(list(pixels[start:start + 3]))
                    green = srgb((frame_id * 73) % 256)
                    expected = [[255, green, 0], [0, green, 255]]
                    measurements.append({'frame': frame_id, 'expected': expected, 'decoded': eyes,
                                         'match': all(abs(actual - want) <= 8 for colors, reference in zip(eyes, expected)
                                                      for actual, want in zip(colors, reference))})
            result['frame_id_measurements'] = measurements
            if len(measurements) != args.frames or not all(item['match'] for item in measurements):
                raise RuntimeError('Decoded image identity does not match its submitted frame; see frame_id_measurements')
            result['stereo_frame_ids_verified'] = True
        if args.quad:
            # Packet delivery can pass while a broken compositor draws black.
            # PyAV is a test-only dependency for checking the actual bitstream.
            import av
            with av.open(str(out / ('stream.' + args.codec))) as capture:
                decoded = list(capture.decode(video=0))
            if len(decoded) != args.frames:
                raise RuntimeError('Decoded frame count differs from delivered frame count')
            rgb = decoded[min(2, args.frames - 1)].reformat(format='rgb24')
            plane = rgb.planes[0]
            pixels = bytes(plane)

            def pixel(eye, x):
                left, right, offset = (.8, .7, -.032) if eye == 0 else (.7, .8, .032)
                px = round((math.tan(left) * args.fov_scale + (x - offset) / 2) / ((math.tan(left) + math.tan(right)) * args.fov_scale) * 1280)
                py = round((math.tan(.75) * args.fov_scale - .6 / 2) / ((math.tan(.75) + math.tan(.65)) * args.fov_scale) * 1344)
                start = py * plane.line_size + (eye * 1280 + px) * 3
                return tuple(pixels[start:start + 3])

            def green(color):
                # Straight-alpha green at alpha .5 becomes ~188 in the sRGB
                # output; this also catches repeated in-place premultiplication.
                return len(color) == 3 and 140 < color[1] < 215 and color[0] < 40 and color[2] < 40

            if not all(green(pixel(eye, x)) for eye, x in [(0, -.8), (1, 0), (0, .8), (1, .8)]):
                raise RuntimeError('Quad geometry/color/alpha failed decoded-pixel validation')
            if not all(max(pixel(eye, x)) < 30 for eye, x in [(1, -.8), (0, 0)]):
                raise RuntimeError('Quad eye visibility failed decoded-pixel validation')
            result['quad_pixels_verified'] = True
            result['quad_eye_visibility_verified'] = True
        result['success'] = True
    except Exception as error:
        result['error'] = str(error)
    finally:
        for child in reversed(children):
            if child.poll() is None:
                child.terminate()
            child.wait(timeout=5)
        for handle in files:
            handle.close()
        (out / 'summary.json').write_text(json.dumps(result, indent=2))
    print(json.dumps({key: value for key, value in result.items() if key != 'frame_id_measurements'}))
    return 0 if result['success'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
