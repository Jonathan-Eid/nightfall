"""Measure PCVR decoder and projection-copy cadence through the dev bridge.

Software counters do not measure display scanout or motion-to-photon latency.
Explicit timing overrides are restored if the same session is still active.
"""
import argparse
import json
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from nf import request


def evaluate(expr, port):
    reply = request({'cmd': 'eval', 'expr': expr}, port=port, timeout=5)
    if not reply.get('ok'):
        raise RuntimeError(reply)
    return reply['value']


def collect(args):
    samples, overrides = [], []
    session = None
    error = None
    deadline = time.monotonic() + args.seconds
    try:
        while time.monotonic() < deadline:
            start = time.monotonic()
            ticks, status, frames, hz, cap = evaluate(
                '[Time.get_ticks_usec(), pcvr_test.backend.get_test_status(), '
                'Engine.get_process_frames(), display_refresh_rate, Engine.max_fps]', args.port)
            rtt = time.monotonic() - start
            active = status.get('state') == 'running' and status.get('meteor_projection_enabled')
            if active:
                if session is None:
                    session = status['session']
                    for setting, value, old in [
                        ('Engine.set("max_fps", %s)', args.render_cap, cap),
                        ('pcvr_test.backend.set_meteor_decoder_poll_ms(%s)', args.decoder_poll_ms, status.get('decoder_poll_ms')),
                        ('pcvr_test.backend.set_meteor_frame_buffering(%s)',
                         None if args.buffered is None else args.buffered == 'on', status.get('buffered_frames')),
                    ]:
                        if value is None:
                            continue
                        if old is None:
                            raise RuntimeError('Diagnostic requires the updated native client')
                        result = evaluate(setting % json.dumps(value), args.port)
                        if result is False:
                            raise RuntimeError('Native timing override was rejected')
                        overrides.append(setting % json.dumps(old))
                    continue
                if status['session'] != session:
                    raise RuntimeError('PCVR session changed during measurement')
                samples.append(dict(ticks_us=ticks, status=status, app_frames=frames,
                                    display_hz=hz, cap=cap, request_seconds=rtt))
            elif session is not None:
                break
            time.sleep(max(0, .5 - rtt))
    except (OSError, RuntimeError, KeyError) as exc:
        error = str(exc)
    finally:
        for expr in reversed(overrides):
            try:
                evaluate(f'{expr} if pcvr_test.backend.get_test_status().get("session") == {session} '
                         'and settings_controller.pcvr_timing.active else null', args.port)
            except (OSError, RuntimeError) as exc:
                error = f'{error or ""}; could not confirm override restoration: {exc}'
    return samples, error


def summarize(samples, error):
    summary = {'samples': len(samples), 'error': error,
               'measurement': 'decoder and swapchain copies; not scanout or motion-to-photon latency'}
    if len(samples) < 2:
        return summary
    first, last = samples[0], samples[-1]
    seconds = (last['ticks_us'] - first['ticks_us']) / 1e6
    a, b = first['status']['meteor_device_counts'], last['status']['meteor_device_counts']
    intervals = [(v['status']['frames'] - u['status']['frames']) /
                 ((v['ticks_us'] - u['ticks_us']) / 1e6) for u, v in zip(samples, samples[1:])]
    timings = {}
    for name, end in last['status'].get('meteor_device_timing_us', {}).items():
        begin = first['status']['meteor_device_timing_us'][name]
        count = end['count'] - begin['count']
        timings[name] = dict(mean_us=(end['sum_us'] - begin['sum_us']) / count if count else None,
                             samples=count, cumulative=end)
    summary.update(seconds=seconds, display_hz=last['display_hz'], cap=last['cap'],
        app_fps=(last['app_frames'] - first['app_frames']) / seconds,
        rates={key: (b[key] - a[key]) / seconds for key in b},
        video_width=last['status']['video_width'], video_height=last['status']['video_height'],
        decoder_poll_ms=last['status'].get('decoder_poll_ms'), buffered=last['status'].get('buffered_frames'),
        device_timing_us=timings, interval_copy_fps=intervals)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=float, default=35)
    parser.add_argument('--port', type=int, default=47950)
    parser.add_argument('--render-cap', type=int, choices=[0, 72, 90])
    parser.add_argument('--decoder-poll-ms', type=int, choices=range(1, 11))
    parser.add_argument('--buffered', choices=['on', 'off'])
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 600:
        parser.error('seconds must be 1..600')
    samples, error = collect(args)
    summary = summarize(samples, error)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({'summary': summary, 'samples': samples}, indent=2), encoding='utf-8')
    print(json.dumps({k: v for k, v in summary.items() if k != 'interval_copy_fps'}, indent=2))
    return 0 if len(samples) >= 2 and not error else 1


if __name__ == '__main__':
    raise SystemExit(main())
