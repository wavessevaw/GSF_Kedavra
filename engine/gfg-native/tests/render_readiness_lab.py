#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Inject a producer dependency in a real Vulkan APP, never in the GSF layer.

Software reproduction of CPU Present preceding GPU input readiness. This is
not a shader-cost benchmark, GPU saturation, Deck root-cause proof or scanout.
"""
import argparse
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
from recovery_report import recovery_report


def numbers(line):
    return {k: float(v) for k, v in re.findall(r'(\w+)=(-?\d+(?:\.\d+)?)', line)}


def median(values):
    return round(statistics.median(values), 4) if values else None


def analyze(events, start, end):
    origins = {(p['pid'], p['generation'], p['te_sequence']): p for p in events
               if p.get('event') == 'pair' and p.get('dispatched_outputs')}
    records = []
    for e in events:
        if e.get('event') != 'gpu_correlated' or e.get('calibration_status') != 'calibrated':
            continue
        p = origins.get((e['pid'], e['generation'], e['te_sequence']))
        if p is None:
            raise ValueError('Unmatched calibrated GPU observation')
        frame = p['source1_id']-1
        if start <= frame < end:
            records.append((p, e))
    pairs = [p for p in events if p.get('event') == 'pair' and start <= p['source1_id']-1 < end]
    return {'source_pairs': len(pairs),
            'dispatched': sum(p['dispatched_outputs'] for p in pairs),
            'wsi_accepted': sum(bool(p['wsi_accepted_ns'][0]) for p in pairs),
            'calibrated_records': len(records),
            'gpu_span_median_ms': median([e['ready_ms'][0] for e in events
                if e.get('event') == 'gpu_pair' and
                (p := origins.get((e['pid'], e['generation'], e['te_sequence']))) is not None
                and start <= p['source1_id']-1 < end and e.get('timed')]),
            'submit_to_gpu_start_median_ms': median([e['submit_to_gpu_start_ns']/1e6 for _, e in records]),
            'source_to_gpu_start_median_ms': median([(e['gpu_start_cpu_ns']-p['source1_ns'])/1e6 for p, e in records]),
            'source_to_compute_admission_median_ms': median([(p['compute_admission_ns']-p['source1_ns'])/1e6
                for p in pairs if p.get('compute_admission_ns')])}


def run(build, output, order):
    output.mkdir(parents=True, exist_ok=True)
    cases = [
        # name, layer, admission, requested source Hz, frames, delay ms/range
        ('healthy', True, True, 5, 30, 0, 0, 0),
        ('healthy-observed', True, True, 5, 30, 0, 0, 0),
        ('refresh-wsi-query', True, True, 5, 12, 0, 0, 0),
        ('baseline-delayed', False, False, 45, 180, 35, 0, 180),
        ('old-admission-delayed', True, False, 45, 180, 35, 0, 180),
        ('new-admission-delayed', True, True, 45, 180, 35, 0, 180),
        # llvmpipe LMC costs ~90 ms in the first experiment: no healthy
        # 45 Hz baseline existed to recover to. Use the same correctness
        # rate as healthy, prove generation BEFORE the delay, then remove
        # only that dependency. This is not a Deck performance setting.
        ('delay-then-recovery', True, True, 5, 90, 150, 20, 45),
    ]
    result = {'scope': 'SOFTWARE_APP_RENDER_DEPENDENCY_NOT_DECK_OR_SCANOUT',
              'order': order, 'cases': [], 'status': 'FAIL'}
    try:
        for name, active, admission, hz, frames, delay_ms, start, end in cases:
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith(('GFG_NATIVE_', 'GFG_TEST_', 'DISABLE_GFG_NATIVE_'))}
            env.update(VK_LAYER_PATH=str(build)+':/usr/share/vulkan/explicit_layer.d',
                       GFG_TEST_VALIDATION='1', GFG_TEST_FRAMES=str(frames),
                       GFG_TEST_SOURCE_HZ=str(hz), GFG_TEST_ABSOLUTE_PACING='1',
                       GFG_TEST_ALLOW_SKIPS='1', GFG_TEST_WIDTH='96', GFG_TEST_HEIGHT='64',
                       GFG_TEST_RENDER_READY_DELAY_MS=str(delay_ms),
                       GFG_TEST_RENDER_READY_FROM=str(start), GFG_TEST_RENDER_READY_UNTIL=str(end))
            layers = ['VK_LAYER_GFG_native', 'VK_LAYER_KHRONOS_validation'] if active else ['VK_LAYER_KHRONOS_validation']
            if order == 'validation-first':
                layers.reverse()
            env['VK_INSTANCE_LAYERS'] = ':'.join(layers)
            trace = output/(name+'.jsonl')
            app_trace = output/(name+'-app.jsonl')
            env['GFG_TEST_RENDER_READY_TRACE'] = str(app_trace)
            if active:
                env.update(GFG_TEST_EXPECT_GPU='1', GFG_NATIVE_FG='1', GFG_NATIVE_GPU_BRIDGE='1',
                           GFG_NATIVE_GPU_SYNC='same-device', GFG_NATIVE_ASYNC='1',
                           GFG_NATIVE_FORCE_FIFO='1', GFG_NATIVE_MODE='2', GFG_NATIVE_ALGO='lmc-gpu',
                           GFG_NATIVE_LMC_TIER='2', GFG_NATIVE_PACING='1', GFG_NATIVE_OUTPUT_HZ='90',
                           GFG_NATIVE_VISIBILITY_PROBE='1', GFG_NATIVE_TIMELINE_WATCH='1',
                           GFG_NATIVE_CALIBRATED_TIMESTAMPS='1',
                           GFG_NATIVE_QUEUE_OBSERVE='0' if name in ('healthy', 'refresh-wsi-query') else '1',
                           GFG_NATIVE_LATENCY_ADMISSION='1' if admission else '0',
                           GFG_NATIVE_TELEMETRY=str(trace))
            if name == 'refresh-wsi-query':
                env.pop('GFG_NATIVE_OUTPUT_HZ', None)
            p = subprocess.run([str(build/'wsi-gpu-smoke')], env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, timeout=90)
            (output/(name+'.log')).write_text(p.stdout)
            assert p.returncode == 0, (name, p.stdout[-7000:])
            assert 'validation errors=0' in p.stdout, name
            row = {'name': name, 'frames': frames, 'source_hz': hz,
                   'gate_delay_ms': delay_ms, 'gate_from': start, 'gate_until': end}
            # Retain the case even if an assertion below fails.
            result['cases'].append(row)
            row['source_rate'] = numbers(next(l for l in p.stdout.splitlines() if l.startswith('GSF_SOURCE_RATE ')))
            app_events = [json.loads(l) for l in app_trace.read_text().splitlines()]
            source_markers = [m for m in app_events if m['event'] == 'source']
            assert len(source_markers) == frames, (name, 'Incomplete application evidence')
            row['application_submit_to_present_median_ms'] = median(
                [(m['present_ns']-m['submit_ns'])/1e6 for m in source_markers if m['injected_delay_ms']])
            if end:
                gate = next(m for m in app_events if m['event'] == 'gate_summary')
                assert gate['ok'] and gate['armed'] == gate['released'] == end-start, (name, gate)
            if active:
                stats = numbers(next(l for l in p.stdout.splitlines() if l.startswith('GFG native: gpu stats ')))
                assert stats['real'] == frames and stats['errors'] == 0, (name, stats)
                assert stats['present_path_host_pixel_reads'] == stats['present_path_cpu_waits'] == 0, stats
                events = [json.loads(l) for l in trace.read_text().splitlines()]
                row.update(stats=stats, all=analyze(events, 0, frames))
                for e in events:
                    if e.get('event') == 'gpu_correlated' and e.get('calibration_status') == 'calibrated':
                        assert e['submit_to_gpu_start_ns'] == max(0, e['gpu_start_minus_submit_return_ns']), e
                contexts = {(e['pid'], e['generation']): e for e in events if e.get('event') == 'bridge_context'}
                pairs = [e for e in events if e.get('event') == 'pair']
                assert pairs and all((e['pid'], e['generation']) in contexts for e in pairs), name
                assert all(c['source_timestamp_semantics'] == 'cpu_vulkan_present_entry' and
                           c['source_gpu_ready_ns'] is None and c['physical_display_hz'] is None and
                           c['te_uses_present_queue'] for c in contexts.values()), name
                if name == 'refresh-wsi-query':
                    for e in pairs:
                        assert e['refresh_override_present'] == 0, e
                        if e['refresh_query_state'] == 'accepted':
                            assert e['refresh_query_result'] == 0 and e['refresh_query_raw_ns'] == e['refresh_period_ns'], e
                            assert e['refresh_origin'] == 'successful_wsi_query', e
                        else:
                            assert e['refresh_query_state'] in ('unavailable', 'query_failed', 'rejected_range'), e
                    row['refresh_query_states'] = sorted({e['refresh_query_state'] for e in pairs})
                else:
                    assert all(e['refresh_origin'] == 'explicit_override' and
                               e['refresh_query_state'] == 'override_present' and
                               e['refresh_query_result'] is None and
                               e['refresh_period_ns'] == 1000000000//90 for e in pairs), name
                observations = [e for e in events if e.get('event') == 'queue_observation']
                if name in ('healthy', 'refresh-wsi-query'):
                    assert not observations, 'Observer was not disabled'
                else:
                    assert observations, 'No actual TE-submit queue snapshot'
                    origins = {(p['pid'], p['generation'], p['te_sequence']) for p in events
                               if p.get('event') == 'pair' and p.get('dispatched_outputs')}
                    assert all((e['pid'], e['generation'], e['te_sequence']) in origins for e in observations), name
                    assert all(e['available'] and e['query_errors'] == 0 for e in observations), name
                    assert all(e['app_submits_total'] <= frames for e in observations), 'GSF counted as game work'
                    assert all(e['hardware_queue_depth'] is None and e['pure_queue_wait_ns'] is None for e in observations)
                    row['queue_observation'] = {'samples': len(observations),
                        'query_cpu_median_ms': median([e['query_cpu_ns']/1e6 for e in observations]),
                        'unretired_upper_bound_median': median([e['app_submits_unretired_upper_bound'] for e in observations]),
                        'incomplete_samples': sum(bool(e['tracking_lost'] or e['unfenced_unretired']) for e in observations)}
                if name != 'refresh-wsi-query':
                    assert row['all']['calibrated_records'] > 0, 'Calibration unavailable; cannot establish this experiment'
                if name == 'refresh-wsi-query':
                    pass  # Refresh provenance only; no software-GPU throughput threshold.
                elif name.startswith('healthy'):
                    assert stats['synthetic'] >= 10, row
                elif name.endswith('delayed'):
                    row['steady'] = analyze(events, 60, frames)
                    # Require genuinely asynchronous producer readiness, not
                    # an app-side vkQueueSubmit wait disguised as GPU latency.
                    assert row['application_submit_to_present_median_ms'] < 17.5, row
                    # Drivers may absorb the dependency in the layer's source
                    # copy submit instead of after TE submit. Preserve and
                    # classify that difference; never call it the Deck result.
                    gpu = row['steady']['submit_to_gpu_start_median_ms']
                    cpu = row['steady']['source_to_compute_admission_median_ms']
                    row['wait_location'] = ('after_TE_submit' if gpu is not None and gpu >= 20
                                            else 'before_TE_submit' if cpu is not None and cpu >= 20
                                            else 'not_reproduced')
                    assert row['wait_location'] != 'not_reproduced', row
                    if admission:
                        assert row['steady']['dispatched'] <= 4, row
                    else:
                        assert row['steady']['dispatched'] >= 8, row
                else:
                    row['before_delay'] = analyze(events, 5, start)
                    row['delayed'] = analyze(events, start, end)
                    row['recovered'] = analyze(events, 75, frames)
                    row['recovery_status'] = recovery_report(events, end, 75)
                    assert row['before_delay']['wsi_accepted'] >= 3, ('No viable recovery baseline', row)
                    assert row['delayed']['wsi_accepted'] == 0, ('Dependency did not suppress generation', row)
                    assert row['recovered']['wsi_accepted'] >= 3, row
                    assert row['recovery_status']['status'] == 'recovered', row
            (output/'RESULT.json').write_text(json.dumps(result, indent=2)+'\n')
            print('READINESS_CASE', json.dumps(row), flush=True)
        delayed = [c for c in result['cases'] if c['name'] in ('old-admission-delayed', 'new-admission-delayed')]
        result['deck_gpu_delay_mechanism_reproduced'] = all(c['wait_location']=='after_TE_submit' for c in delayed)
        result['status'] = 'PASS'
    except Exception as e:
        result['error'] = str(e)
        raise
    finally:
        (output/'RESULT.json').write_text(json.dumps(result, indent=2)+'\n')
    print('RENDER_READINESS_LAB_PASS: real app dependency, preserved originals, calibrated latency, bounded retries, recovery; Deck cause unproven')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', required=True, type=Path)
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--order', choices=('native-first', 'validation-first'), required=True)
    a = p.parse_args()
    run(a.build.resolve(), a.out.resolve(), a.order)
