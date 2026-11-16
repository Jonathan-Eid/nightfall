"""Read ALVR's local event socket for a bounded, reproducible comparison.

Requires websocket-client. This does not launch apps or change ALVR settings.
ALVR's pipeline timing estimates are not a physical motion-to-photon test.
"""
import argparse
from contextlib import closing
import json
import math
from pathlib import Path
import statistics
import time

import websocket


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=float, default=30)
    parser.add_argument('--warmup', type=float, default=0)
    parser.add_argument('--port', type=int, default=8082)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 600 or not 0 <= args.warmup <= 120:
        parser.error('seconds must be 1..600 and warmup 0..120')
    graphs, summaries, logs = [], [], []
    error = None
    start = time.monotonic()
    try:
        with closing(websocket.create_connection(f'ws://127.0.0.1:{args.port}/api/events',
                header=['X-ALVR: true'], suppress_origin=True, timeout=2)) as sock:
            while time.monotonic() - start < args.seconds + args.warmup:
                try:
                    event = json.loads(sock.recv())
                except websocket.WebSocketTimeoutException:
                    continue
                item = event['event_type']
                if time.monotonic() - start < args.warmup:
                    continue
                if item['id'] == 'GraphStatistics':
                    graphs.append(item['data'])
                elif item['id'] == 'StatisticsSummary':
                    summaries.append(item['data'])
                elif item['id'] == 'Log' and len(logs) < 100:
                    logs.append(item['data'])
    except (OSError, ValueError, KeyError, websocket.WebSocketException) as exc:
        error = str(exc)
    summary = {'samples': len(graphs), 'error': error,
               'measurement': 'ALVR software pipeline estimates; not physical scanout'}
    if graphs:
        for key in graphs[0]:
            values = [g[key] for g in graphs if isinstance(g.get(key), (int, float))
                      and math.isfinite(g[key])]
            if values:
                values.sort()
                summary[key] = {'mean': statistics.mean(values), 'median': statistics.median(values),
                                'p95': values[max(0, math.ceil(.95 * len(values)) - 1)]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({'summary': summary, 'graphs': graphs,
                                      'statistics': summaries, 'logs': logs}, indent=2), encoding='utf-8')
    print(json.dumps(summary, indent=2))
    return 0 if len(graphs) >= 2 and not error else 1


if __name__ == '__main__':
    raise SystemExit(main())
