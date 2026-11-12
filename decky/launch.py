#!/usr/bin/env python3
"""Per-game GSF launcher. Disabled/uninstalled profiles are strict pass-through."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import time

from gsf_runtime import Runtime, app_id, atomic_json, candidate_module, gate_rejection, live_records, load_json

LOG_LIMIT = 16*1024*1024
TELEMETRY_LIMIT = 32*1024*1024


def game_environment(kit, run, inherited, diagnostic_mode="normal"):
    # Call before changing the environment: use the stock WSI manifest and
    # disable switch exactly as in the controlled Game Mode qualification.
    base = candidate_module(kit)
    order = base.prepare_gamescope_layer(run)
    env = {k: v for k, v in inherited.items() if not k.startswith(('GFG_NATIVE_', 'DISABLE_GFG_NATIVE_'))}
    layers = [p for p in inherited.get('VK_INSTANCE_LAYERS', '').split(':') if p]
    if any(any(tag in name.lower() for tag in ('gfg', 'lsfg', 'mako')) for name in layers):
        raise ValueError('Другой генератор уже подключён к этой игре')
    paths = [str(kit/'layers')]
    if order:
        paths.append(str(order['directory']))
        env.update(order['disable_environment'])
    for key in ('VK_LAYER_PATH', 'VK_ADD_LAYER_PATH'):
        paths.extend(p for p in inherited.get(key, '').split(':') if p)
    for base_path in (inherited.get('XDG_CONFIG_HOME', str(Path.home()/'.config')),
                      inherited.get('XDG_DATA_HOME', str(Path.home()/'.local/share')),
                      *inherited.get('XDG_CONFIG_DIRS', '/etc/xdg').split(':'),
                      *inherited.get('XDG_DATA_DIRS', '/usr/local/share:/usr/share').split(':'),
                      '/etc', '/usr/share'):
        if base_path:
            paths.append(base_path+'/vulkan/explicit_layer.d')
    env['VK_LAYER_PATH'] = ':'.join(dict.fromkeys(paths))
    frog = 'VK_LAYER_FROG_gamescope_wsi_x86_64'
    layers = ['VK_LAYER_GFG_native']+[p for p in layers if p != frog]+([frog] if order else [])
    env['VK_INSTANCE_LAYERS'] = ':'.join(layers)
    env.update(GFG_NATIVE_FG='1', GFG_NATIVE_GPU_BRIDGE='1', GFG_NATIVE_GPU_SYNC='same-device',
               GFG_NATIVE_ASYNC='1', GFG_NATIVE_FORCE_FIFO='1', GFG_NATIVE_MODE='2',
               GFG_NATIVE_ALGO='lmc-gpu', GFG_NATIVE_LMC_TIER='2', GFG_NATIVE_PACING='1', GFG_NATIVE_VISIBILITY_PROBE='1', GFG_NATIVE_REAL_WSI_PROBE='1',
               GFG_NATIVE_TIMING='1', GFG_NATIVE_TIMELINE_WATCH='1', GFG_NATIVE_CALIBRATED_TIMESTAMPS='1', GFG_NATIVE_LATENCY_ADMISSION='1', GFG_NATIVE_STATUS_DIR=str(run/'status'),
               GFG_NATIVE_TELEMETRY=str(run/'frames.jsonl'))
    if diagnostic_mode not in ("normal", "normal-queue-observe", "normal-no-watch", "normal-no-calibration", "normal-calibration-every-frame", "normal-no-latency-admission", "source-only", "compute-only", "matched-compute"):
        raise ValueError("Unsupported GSF diagnostic mode")
    if diagnostic_mode not in ("normal", "normal-queue-observe", "normal-no-watch", "normal-no-calibration", "normal-calibration-every-frame", "normal-no-latency-admission"):
        env["GFG_NATIVE_DIAGNOSTIC_MODE"] = diagnostic_mode
    if diagnostic_mode == "normal-queue-observe":
        env["GFG_NATIVE_QUEUE_OBSERVE"] = "1"
    if diagnostic_mode == "normal-no-watch":
        env["GFG_NATIVE_TIMELINE_WATCH"] = "0"
    if diagnostic_mode == "normal-no-latency-admission":
        env["GFG_NATIVE_LATENCY_ADMISSION"] = "0"
    if diagnostic_mode == "normal-no-calibration":
        env["GFG_NATIVE_CALIBRATED_TIMESTAMPS"] = "0"
    if diagnostic_mode == "normal-calibration-every-frame":
        env["GFG_NATIVE_CALIBRATION_EVERY_FRAMES"] = "1"
    # Never set SOURCE_HZ, OUTPUT_HZ, a game limiter, pixel readbacks or test badges.
    return env


def run_game(root, aid, command):
    runtime = Runtime(Path.home(), root)
    runtime.root = root
    runtime.results.mkdir(parents=True, exist_ok=True, mode=0o700)
    run = Path(tempfile.mkdtemp(prefix='game-'+aid+'-'+datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ-'),
                                dir=runtime.results))
    (run/'status').mkdir(mode=0o700)
    mode = load_json(root/'config.json', {'games': {}}).get('games', {}).get(aid, {}).get('diagnostic_mode', 'normal')
    report = {'schema': 1, 'diagnostic_mode': mode, 'appid': int(aid), 'stage': 'preflight', 'multiplier': 2,
              'physical_scanout': 'UNVERIFIED', 'result': 'NO_GENERATION_EVIDENCE',
              'last_live': [], 'max_generated_wsi_accepted': 0, 'source_fps_target': None}
    code = 2
    try:
        if platform.machine() != 'x86_64':
            raise ValueError('Пакет предназначен для Linux x86_64')
        current = load_json(root/'current.json')
        kit = Path(current['kit'])
        candidate_module(kit).verify_payload(kit)
        report['build'] = load_json(kit/'BUILD_INFO.json')
        env = game_environment(kit, run, os.environ, mode)
        # No full argv or inherited environment dump: these may contain secrets.
        atomic_json(run/'LAUNCH.json', {'appid': int(aid), 'program': Path(command[0]).name,
                    'build': report['build'], 'native_environment':
                    {k: v for k, v in env.items() if k.startswith('GFG_NATIVE_')}})
        report['stage'] = 'running'
        atomic_json(run/'REPORT.json', report)
        # Direct O_APPEND output avoids pipe deadlocks when Proton spawns a
        # descendant which outlives its initial launcher process.
        from gsf_amdgpu import GpuSampler
        sensor = GpuSampler(run/'amdgpu.jsonl')
        sensor.start()
        with (run/'game.log').open('ab', buffering=0) as output:
            child = subprocess.Popen(command, env=env, stdout=output, stderr=subprocess.STDOUT)
        while child.poll() is None:
            records = live_records(run)
            if records:
                report['last_live'] = records
                generated = max(int(r.get('generated_wsi_accepted', 0)) for r in records)
                report['max_generated_wsi_accepted'] = max(report['max_generated_wsi_accepted'], generated)
                if generated:
                    report['result'] = 'GENERATION_OBSERVED'
            telemetry = run/'frames.jsonl'
            if telemetry.exists() and telemetry.stat().st_size > TELEMETRY_LIMIT:
                with telemetry.open('r+b') as f:
                    f.truncate(0)  # Native writer uses O_APPEND, so next records start at zero.
                report['telemetry_truncated'] = True
            log = run/'game.log'
            if log.stat().st_size > LOG_LIMIT:
                with log.open('r+b') as f:
                    f.truncate(0)
                report['game_log_truncated'] = True
            atomic_json(run/'REPORT.json', report)
            time.sleep(0.25)
        code = child.returncode
        # Preserve fatal errors and retired counters even when the log cap was reached.
        report['stage'] = 'finished'
        report['exit_code'] = code
        if code:
            report['result'] = 'GAME_EXIT_ERROR'
    except Exception as error:
        report['stage'] = 'failed'
        report['error'] = str(error)
        print('GSF: '+str(error), file=sys.stderr)
    finally:
        if 'sensor' in locals():
            sensor.stop()
        report['gate_rejection'] = gate_rejection(run)
        try:
            from gsf_diagnostic import diagnostic_summary
            report['timeline_diagnostic'] = diagnostic_summary(run/'frames.jsonl')
        except ImportError:
            report['timeline_diagnostic'] = {'available': False}
        if not report['max_generated_wsi_accepted'] and report['gate_rejection']:
            report['result'] = 'GENERATION_BLOCKED'
        report['finished_unix_ms'] = int(time.time()*1000)
        atomic_json(run/'REPORT.json', report)
        try:
            runtime.export(aid)
        except OSError as error:
            print('GSF report: '+str(error), file=sys.stderr)
    return code if code >= 0 else 128-code


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--appid', required=True)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('missing command')
    root = Path(__file__).resolve().parent
    aid = app_id(args.appid)
    try:
        config = load_json(root/'config.json', {'games': {}})
        enabled = config.get('games', {}).get(aid, {}).get('enabled') is True
    except (OSError, ValueError, TypeError):
        enabled = False
    if not enabled:
        os.execvpe(command[0], command, os.environ.copy())
    return run_game(root, aid, command)


if __name__ == '__main__':
    sys.exit(main())
