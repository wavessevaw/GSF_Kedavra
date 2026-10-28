import {DialogButton, Focusable, PanelSection, PanelSectionRow, SliderField, ToggleField, gamepadDialogClasses, gamepadSliderClasses, staticClasses} from '@decky/ui';
import {callable, definePlugin, toaster} from '@decky/api';
import {CSSProperties, Fragment, ReactNode, useEffect, useRef, useState} from 'react';
import {LOGO} from './logo';

const VERSION = '1.4.2';

type Summary = {pass?: boolean; p95_ms?: number; p99_ms?: number; missed_pct?: number; rows?: number; checks?: Record<string, boolean>};
type Report = {trace: string; report: string; ok: boolean; log: string; summary?: Summary};
type Capture = {path: string; started: number; label: string; elapsed_s: number} | null;
type Live = {fps: number | null; real: number | null; pace: number | null; gen_share: number | null; unsupported?: number};
type Status = {
  engine: 'running' | 'stale' | 'absent'; fresh: boolean; message?: string; capture: Capture; live?: Live;
  multiplier?: number; state?: string; refresh_ns?: number; width?: number; height?: number;
  steam_ui?: boolean; sources?: number; real?: number; gen?: number; repeat?: number; skip?: number; resync?: number; degrade?: number;
  applied?: number; bypass?: number; ring_miss?: number; unsupported?: number;
  gpu_us_last?: number; gpu_us_avg?: number; gpu_us_max?: number; over_budget?: number; timestamps?: boolean; last_problem?: string;
  auto?: boolean; auto_switches?: number; source_late?: number; cap_fps?: number; base_fps?: number;
  unsupported_format?: number; unsupported_colorspace?: number;
  auto_floor?: number; hdr_enabled?: boolean; transfer?: string; vrr_held?: boolean;
};

// ×2, ×2.5, ×3.75 exactly (quarter steps, like the HUD), else one decimal (×2.6 for 35 at 90 Hz).
const multText = (ratio: number) => Math.abs(ratio - Math.round(ratio)) < 0.05 ? `×${Math.round(ratio)}`
  : Math.abs(ratio * 4 - Math.round(ratio * 4)) < 0.01 ? `×${(Math.round(ratio * 4) / 4).toFixed(2).replace(/0$/, '')}` : `×${ratio.toFixed(1)}`;
type Mode = 'off' | 'auto' | 'cap';
type View = 'main' | 'setup' | 'pace' | 'log';
type Check = {key: string; ok: boolean; text: string; detail: string};
type Diagnose = {checks: Check[]; ok: boolean};
type Bench = {ok: boolean; correct?: boolean; analysis_ms?: number; two_outputs_ms?: number; device?: string; log: string; json: string};

const rpc = {
  status: callable<[], Status>('get_status'),
  settings: callable<[], {hud: boolean; base_fps: number; x375: boolean; hdr: boolean}>('get_settings'),
  setOptions: callable<[boolean, boolean], {x375: boolean; hdr: boolean}>('set_options'),
  setMode: callable<[Mode, number], {mode: Mode; base_fps: number}>('set_mode'),
  setHud: callable<[boolean], {hud: boolean}>('set_hud'),
  start: callable<[string], Capture>('start_capture'),
  stop: callable<[], Report>('stop_capture'),
  logs: callable<[], string>('get_logs'),
  diagnose: callable<[], Diagnose>('diagnose'),
  report: callable<[], {path: string}>('save_report'),
  install: callable<[], {message: string; needs_reboot: boolean}>('install'),
  uninstall: callable<[], {message: string; needs_reboot: boolean}>('uninstall'),
  reboot: callable<[], unknown>('reboot'),
  bench: callable<[], Bench>('run_bench'),
};

// Green-black palette shared with the HUD (kedavra_hud.py) and the README.
const C = {
  ground: '#050706', card: '#0d1210', line: '#1c261f', track: '#1a231d',
  green: '#52d64a', greenText: '#8ff07a', greenBg: '#13301a',
  yellow: '#e3c04a', red: '#ff8a7a', redBg: '#3a1d1a',
  text: '#f2f6f3', muted: '#a9b5ae', label: '#8e9b93',
};
// One grid for the whole panel: cards share the row's edges, everything inside a card starts
// 14 px in, rows are 10 px apart, corners are 14 px.
const INSET = 14;
// Cards: a faint top-left light on near-black, like GFG Extreme's panels.
const CARD_BG = 'linear-gradient(160deg, #131c16 0%, #0b100d 60%, #090d0b 100%)';
const card: CSSProperties = {background: CARD_BG, border: `1px solid ${C.line}`, borderRadius: 14, padding: INSET, boxSizing: 'border-box'};
const glow = (hex: string, r = 8) => `drop-shadow(0 0 ${r}px ${hex}66)`;
const caps: CSSProperties = {fontSize: 10, fontWeight: 700, letterSpacing: '0.16em', color: C.label};

function Ring({size, width, frac, color, children}: {size: number; width: number; frac: number; color: string; children?: ReactNode}) {
  const r = (size - width) / 2;
  const len = 2 * Math.PI * r;
  const f = Math.max(0, Math.min(1, frac));
  return <div style={{position: 'relative', width: size, height: size, flex: 'none'}}>
    <svg width={size} height={size} viewBox={`0 0 ${size} ${size}`} style={{overflow: 'visible'}}>
      <circle cx={size / 2} cy={size / 2} r={r} fill="none" stroke={C.track} strokeWidth={width}/>
      {f > 0 && <circle cx={size / 2} cy={size / 2} r={r} fill="none" stroke={color} strokeWidth={width} strokeLinecap="round"
        strokeDasharray={`${f * len} ${len}`} transform={`rotate(-90 ${size / 2} ${size / 2})`} style={{filter: glow(color, width)}}/>}
    </svg>
    <div style={{position: 'absolute', inset: 0, display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center'}}>{children}</div>
  </div>;
}

function Back({onClick, title}: {onClick: () => void; title: string}) {
  return <PanelSectionRow><Focusable style={{display: 'flex', alignItems: 'center', gap: 10}}>
    <DialogButton onClick={onClick} className="gsf-row" style={{minWidth: 0, width: 40, height: 36, padding: 0, borderRadius: 10, background: C.card, border: `1px solid ${C.line}`}}>‹</DialogButton>
    <div style={{fontSize: 18, fontWeight: 800}}>{title}</div>
  </Focusable></PanelSectionRow>;
}


// Flat buttons: Steam's DialogButton brings its own grey and padding.
const flat: CSSProperties = {minWidth: 0, border: 'none', boxShadow: 'none'};
const primary: CSSProperties = {...flat, background: '#52d64a', color: '#04120a', fontWeight: 800, borderRadius: 12, padding: '11px 12px'};
const secondary: CSSProperties = {...flat, background: '#0d1210', color: '#f2f6f3', fontWeight: 700, borderRadius: 12, padding: '10px 12px', border: '1px solid #1c261f'};

// Simple line icons (24 px grid, stroke = currentColor) for the tiles.
const ICONS: Record<string, string> = {
  off: 'M12 3v8M6.3 6.3a8 8 0 1 0 11.4 0',
  auto: 'M12 3l2.2 6.8L21 12l-6.8 2.2L12 21l-2.2-6.8L3 12l6.8-2.2z',
  cap: 'M4 7h10M18 7h2M4 17h4M12 17h8M16 4v6M10 14v6',
  pace: 'M3 12h4l2-6 4 12 2-6h6',
  setup: 'M12 8a4 4 0 1 0 0 8 4 4 0 0 0 0-8zM12 2v3M12 19v3M2 12h3M19 12h3M4.9 4.9l2.1 2.1M17 17l2.1 2.1M4.9 19.1L7 17M17 7l2.1-2.1',
  log: 'M6 3h9l4 4v14H6zM9 11h7M9 15h7M9 7h3',
  info: 'M12 8v.5M12 11v6M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18z',
  warn: 'M12 3l10 18H2zM12 10v5M12 18v.5',
};
const Icon = ({name, size = 20}: {name: string; size?: number}) =>
  <svg width={size} height={size} viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth={2} strokeLinecap="round" strokeLinejoin="round"
    style={{display: 'block', flex: 'none'}}><path d={ICONS[name]}/></svg>;

// A big tile with an icon, as in GFG Extreme: modes and the Pace / Setup / Log row.
function Tile({selected, onClick, disabled, label, icon, dot}: {selected?: boolean; onClick: () => void; disabled?: boolean; label: string; icon: string; dot?: boolean}) {
  return <DialogButton onClick={onClick} disabled={disabled} className={'gsf-tile' + (selected ? ' gsf-on' : '')}
    style={{...flat, flex: 1, position: 'relative', display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', gap: 6,
      height: 66, padding: '8px 2px', borderRadius: 14, boxSizing: 'border-box', fontSize: 12, fontWeight: 700,
      background: selected ? 'linear-gradient(180deg, #74ec6a 0%, #45c83d 100%)' : CARD_BG, color: selected ? '#04120a' : C.text,
      border: `1px solid ${selected ? C.green : C.line}`, boxShadow: selected ? '0 0 16px rgba(82, 214, 74, 0.35)' : 'none'}}>
    <Icon name={icon}/>{label}
    {dot && <span style={{position: 'absolute', top: 8, right: 10, width: 7, height: 7, borderRadius: 4, background: C.red}}/>}
  </DialogButton>;
}

// A small card with a ring, as GFG's REAL / POWER / LEFT row.
function Mini({label, value, frac, color}: {label: string; value: string; frac: number; color: string}) {
  return <div style={{...card, flex: 1, minWidth: 0, padding: '10px 0', display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 6}}>
    <Ring size={44} width={4} frac={frac} color={color}><div style={{fontSize: value.length > 3 ? 11 : 12, fontWeight: 800}}>{value}</div></Ring>
    <div style={{...caps, fontSize: 9}}>{label}</div>
  </div>;
}

function ListButton({title, sub, onClick, warn}: {title: string; sub?: string; onClick: () => void; warn?: boolean}) {
  return <DialogButton onClick={onClick} className="gsf-row" style={{...flat, display: 'flex', alignItems: 'center', gap: 10, textAlign: 'left', padding: '10px 14px',
    borderRadius: 12, background: C.card, border: `1px solid ${C.line}`, color: C.text}}>
    <span style={{flex: 1, minWidth: 0}}>
      <span style={{display: 'block', fontSize: 13, fontWeight: 700}}>{title}</span>
      {sub && <span style={{display: 'block', fontSize: 10, color: warn ? C.red : C.label, marginTop: 2}}>{sub}</span>}
    </span>
    <span style={{color: C.label, fontSize: 16}}>›</span>
  </DialogButton>;
}

const fmt = (v: number | null | undefined, d = 0) => v === null || v === undefined ? '—' : v.toFixed(d);

function Content() {
  const [status, setStatus] = useState<Status>();
  const [view, setView] = useState<View>('main');
  const [hud, setHud] = useState(true);
  const [x375, setX375] = useState(false);
  const [hdr, setHdr] = useState(true);
  const [base, setBase] = useState(30);
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState('');
  const [report, setReport] = useState<Report>();
  const [logs, setLogs] = useState('');
  const [diag, setDiag] = useState<Diagnose>();
  const [needsReboot, setNeedsReboot] = useState(false);
  const [benchRes, setBenchRes] = useState<Bench>();
  const [showChecks, setShowChecks] = useState(false);
  const lock = useRef(false);
  const baseTimer = useRef<number | undefined>(undefined);

  useEffect(() => {rpc.settings().then(r => {setHud(r.hud); setBase(r.base_fps); setX375(r.x375); setHdr(r.hdr);}).catch(() => {});}, []);
  const refreshDiag = async () => {try {setDiag(await rpc.diagnose());} catch (e) {setMessage(String(e));}};
  useEffect(() => {void refreshDiag();}, []);
  useEffect(() => {
    let gone = false;
    const refresh = async () => {
      try {const s = await rpc.status(); if (!gone) setStatus(s);}
      catch (e) {if (!gone) setMessage(String(e));}
    };
    void refresh();
    const t = setInterval(refresh, 1000);
    return () => {gone = true; clearInterval(t);};
  }, []);

  const act = async (fn: () => Promise<string>) => {
    if (lock.current) return;
    lock.current = true; setBusy(true);
    try {const text = await fn(); setMessage(text); toaster.toast({title: 'GSF Kedavra', body: text});}
    catch (e) {
      // Errors are shown on the panel and as a toast; a silent failure looks like a dead button.
      const text = 'Error: ' + (e instanceof Error ? e.message : String(e));
      setMessage(text); toaster.toast({title: 'GSF Kedavra', body: text});
    }
    finally {lock.current = false; setBusy(false);}
  };

  const running = status?.engine === 'running';
  const mult = status?.multiplier ?? 1;
  const generating = running && status?.state === 'active';
  const mode: Mode = !running || (mult <= 1 && status?.state !== 'active') ? 'off' : status?.base_fps ? 'cap' : 'auto';
  const hz = status?.refresh_ns ? 1e9 / status.refresh_ns : 90;
  const live = status?.live;
  const pace = live?.pace;
  const paceOk = pace === null || pace === undefined || pace >= 0.995;
  const gameFps = status?.base_fps || status?.cap_fps || 0;
  const ratio = gameFps ? hz / gameFps : mult;
  const installed = diag?.checks.some(c => c.key === 'installed' && c.ok);
  // Auto's caps (kfg_auto.h): refresh / 3 (at least 30), steps of 5, refresh / 2. 90 Hz: 30 / 35 /
  // 40 / 45, and the low floor below them when allowed: 24 or refresh / 4 (90 Hz 24 at ×3.75,
  // 120 Hz 30 at ×4, 60 Hz 24 at ×2.5).
  const hzR = Math.round(hz), capHi = Math.floor(hzR / 2), lo3 = Math.ceil(hzR / 3);
  const capLo = lo3 >= 30 ? lo3 : Math.min(30, capHi);
  const capFloor = Math.max(24, Math.ceil(hzR / 4));
  const floorMult = multText(hzR / capFloor), loMult = multText(hzR / capLo);
  const autoCaps = (() => {
    const lo = capLo, hi = capHi, c = [lo];
    const floor = capFloor;
    if (x375 && floor < lo) c.unshift(floor);
    for (let x = (Math.floor(lo / 5) + 1) * 5; x < hi; x += 5) c.push(x);
    if (hi > lo) c.push(hi);
    return c.join(' / ');
  })();

  const setMode = (m: Mode, b = base) => void act(async () => {
    await rpc.setMode(m, b);
    return m === 'off' ? 'Generation off' : m === 'auto' ? `Auto: the game is held at the highest of ${autoCaps} it keeps steady, ${x375 && capFloor < capLo ? floorMult : loMult} at most`
      : `Base FPS ${b}: the game is held at ${b}, generation fills up to ${Math.round(hz)}`;
  });
  const onBase = (v: number) => {
    setBase(v);
    // The slider fires on every step; apply once it settles.
    window.clearTimeout(baseTimer.current);
    if (mode === 'cap') baseTimer.current = window.setTimeout(() => setMode('cap', v), 500);
  };

  // One short line under the hero card; empty when the card already says it all.
  let note = '';
  if (!running) note = status?.message ?? 'Checking the engine…';
  else if ((live?.unsupported ?? 0) > 1) note = `This game's frames are not supported (Vulkan format ${status?.unsupported_format}, colour space ${status?.unsupported_colorspace}). Save a report from Setup and send it.`;
  else if (status?.steam_ui) note = 'Steam is on screen: generation waits for the game.';
  else if (status?.state === 'degraded') note = 'Paused: frame analysis cannot keep up. Retrying in 2 s.';
  else if (status?.vrr_held) note = 'VRR is paused while generation is on: generated frames need a fixed refresh. Off gives VRR back.';
  else if (mode === 'cap' && live?.real !== null && live?.real !== undefined && live.real < 0.95 * (status?.base_fps ?? 0))
    note = `The game runs below its cap of ${status?.base_fps}. Lower the cap or use Auto for steadier motion.`;
  else if (mode === 'auto') note = (status?.cap_fps ?? 0) >= Math.floor(hz / 2) ? 'Holds the top cap; drops by itself if the game falls behind.'
    : 'Raises the cap by itself when the game has headroom.';

  const allOk = diag?.checks.every(c => c.ok);
  const failed = diag?.checks.filter(c => !c.ok) ?? [];
  const gap: CSSProperties = {height: 6};

  // Above 90 Hz (a TV) the low floor is x4: not recommended, so it lives in Setup, off by default.
  const floorInSetup = hzR > 90;
  const setOptions = (nx: boolean, nh: boolean) => void act(async () => {
    const r = await rpc.setOptions(nx, nh); setX375(r.x375); setHdr(r.hdr);
    return nx !== x375 ? (r.x375 ? `Auto may go down to base ${capFloor} (${floorMult}) when the game cannot hold ${capLo}` : `Auto stays at ${loMult} at most`)
      : (r.hdr ? 'HDR games get generation too' : 'HDR games pass through without generation');
  });

  if (view === 'setup') return <PanelSection>
    <Back onClick={() => setView('main')} title="Setup"/>
    <PanelSectionRow><DialogButton onClick={() => setShowChecks(!showChecks)} className="gsf-row" style={{...card, ...flat, display: 'flex', alignItems: 'center', gap: 12, textAlign: 'left'}}>
      <span style={{width: 30, height: 30, borderRadius: 15, flex: 'none', display: 'flex', alignItems: 'center', justifyContent: 'center',
        background: allOk ? C.greenBg : C.redBg, color: allOk ? C.greenText : C.red, fontWeight: 900}}>{allOk ? '✓' : '!'}</span>
      <span style={{flex: 1, minWidth: 0}}>
        <span style={{display: 'block', fontSize: 14, fontWeight: 700}}>{!diag ? 'Checking the system…' : allOk ? 'System ready' : `${failed.length} check${failed.length > 1 ? 's' : ''} failed`}</span>
        <span style={{display: 'block', fontSize: 11, color: C.muted, marginTop: 2}}>{diag ? `${diag.checks.length - failed.length} of ${diag.checks.length} checks passed` : ''}</span>
      </span>
      <span style={{color: C.label, fontSize: 12}}>{showChecks ? 'Hide' : 'Details'}</span>
    </DialogButton></PanelSectionRow>
    {(showChecks || !allOk) && diag && <PanelSectionRow><div style={{...card, display: 'flex', flexDirection: 'column', gap: 7}}>
      {diag.checks.filter(c => showChecks || !c.ok).map((c, i) => <div key={c.key + i} style={{display: 'flex', gap: 8, fontSize: 11, lineHeight: 1.35, overflowWrap: 'anywhere'}}>
        <span style={{color: c.ok ? C.greenText : C.red, fontWeight: 800}}>{c.ok ? '✓' : '✗'}</span>
        <div style={{color: c.ok ? C.muted : C.text}}>{c.text}{c.detail && <div style={{fontSize: 10, color: C.muted, whiteSpace: 'pre-wrap'}}>{c.detail}</div>}</div>
      </div>)}
    </div></PanelSectionRow>}
    <div style={gap}/>
    <PanelSectionRow><DialogButton disabled={busy} onClick={() => void act(async () => {
      const r = await rpc.install(); setNeedsReboot(r.needs_reboot); await refreshDiag(); return r.message;
    })} className="gsf-btn" style={primary}>{busy ? 'Working…' : installed ? 'Reinstall Gamescope KEDAVRA' : 'Install Gamescope KEDAVRA'}</DialogButton></PanelSectionRow>
    {needsReboot && <PanelSectionRow><DialogButton disabled={busy} onClick={() => void act(async () => {
      await rpc.reboot(); return 'Rebooting…';
    })} className="gsf-btn" style={secondary}>Reboot now</DialogButton></PanelSectionRow>}
    <PanelSectionRow><div style={{fontSize: 11, lineHeight: 1.45, color: C.label, padding: '0 2px'}}>
      Mounted over the stock Gamescope from /var/lib/kedavra; the SteamOS image is not changed. If Gaming Mode fails to start, the next boot uses the stock one.</div></PanelSectionRow>
    <div style={gap}/>
    {floorInSetup && capFloor < capLo && <>
      <PanelSectionRow><div style={caps}>ENGINE</div></PanelSectionRow>
      <PanelSectionRow><div className="gsf-green" style={{...card, padding: `2px ${INSET}px`}}>
        <ToggleField label={`Allow ${floorMult} in Auto`} description={`Not recommended: base ${capFloor} when the game cannot hold ${capLo}`} checked={x375}
          disabled={busy || !running} bottomSeparator="none" onChange={v => setOptions(v, hdr)}/>
      </div></PanelSectionRow>
      <div style={gap}/>
    </>}
    <PanelSectionRow><div style={caps}>TOOLS</div></PanelSectionRow>
    <PanelSectionRow><Focusable style={{display: 'flex', flexDirection: 'column', gap: 6}}>
      <ListButton title="Save report to Desktop" sub="One zip: diagnostics, pacing traces, benchmark" onClick={() => void act(async () => 'Report saved: ' + (await rpc.report()).path.replace(/^.*\/Desktop\//, 'Desktop/'))}/>
      <ListButton title="GPU benchmark" sub={benchRes ? (benchRes.ok ? `Analysis ${benchRes.analysis_ms?.toFixed(1)} ms · two frames ${benchRes.two_outputs_ms?.toFixed(1)} ms · ${benchRes.correct ? 'PASS' : 'FAIL'}` : 'Failed') : 'About 20 seconds'}
        onClick={() => void act(async () => {
          const b = await rpc.bench(); setBenchRes(b);
          return b.ok ? `Benchmark: analysis ${b.analysis_ms?.toFixed(2)} ms, two frames ${b.two_outputs_ms?.toFixed(2)} ms` : 'Benchmark failed';
        })}/>
      <ListButton title="Check again" onClick={() => void act(async () => {await refreshDiag(); return 'Check updated';})}/>
    </Focusable></PanelSectionRow>
    {message && <PanelSectionRow><div style={{fontSize: 11, color: C.muted, overflowWrap: 'anywhere', whiteSpace: 'pre-wrap'}}>{message}</div></PanelSectionRow>}
    {installed && <><div style={gap}/><PanelSectionRow><DialogButton disabled={busy} onClick={() => void act(async () => {
      const r = await rpc.uninstall(); setNeedsReboot(r.needs_reboot); await refreshDiag(); return r.message;
    })} className="gsf-btn" style={{...flat, background: 'transparent', color: C.red, fontSize: 12, padding: '8px 0'}}>Remove and restore stock Gamescope</DialogButton></PanelSectionRow></>}
  </PanelSection>;

  if (view === 'log') return <PanelSection>
    <Back onClick={() => setView('main')} title="Log"/>
    <PanelSectionRow><DialogButton disabled={busy} className="gsf-btn" style={secondary} onClick={() => void act(async () => {setLogs(await rpc.logs()); return 'Log updated';})}>Refresh</DialogButton></PanelSectionRow>
    {logs && <PanelSectionRow><pre style={{...card, fontSize: 9, whiteSpace: 'pre-wrap', maxHeight: 360, overflow: 'auto', margin: 0}}>{logs}</pre></PanelSectionRow>}
  </PanelSection>;

  if (view === 'pace') return <PanelSection>
    <Back onClick={() => setView('main')} title="Pace"/>
    <PanelSectionRow><div style={{fontSize: 12, lineHeight: 1.5, color: C.muted, padding: '0 2px'}}>
      Records the on-screen time of every frame (KMS flip). Play for one or two minutes, then stop.</div></PanelSectionRow>
    <PanelSectionRow>{status?.capture ?
      <DialogButton disabled={busy} onClick={() => void act(async () => {
        const r = await rpc.stop(); setReport(r);
        const s = r.summary;
        return s ? `Trace: P95 ${s.p95_ms?.toFixed(3)} ms, P99 ${s.p99_ms?.toFixed(3)} ms, missed ${s.missed_pct?.toFixed(2)}% → ${s.pass ? 'PASS' : 'FAIL'}` : 'Trace saved, analysis failed';
      })} className="gsf-btn" style={{...primary, background: C.text}}>■  Stop recording · {status.capture.elapsed_s.toFixed(0)} s</DialogButton> :
      <DialogButton disabled={busy || !running} onClick={() => void act(async () => {
        const label = mode === 'cap' ? `cap${status?.base_fps}` : `${status?.auto ? 'auto-' : ''}x${mult}`;
        const c = await rpc.start(label); return 'Recording KMS flip trace: ' + (c?.path ?? '');
      })} className="gsf-btn" style={primary}>●  Record pacing trace</DialogButton>}
    </PanelSectionRow>
    {report?.summary && <PanelSectionRow><Focusable style={{display: 'flex', gap: 8}}>
      {[['P95', `${report.summary.p95_ms?.toFixed(2)} ms`, '≤ 1 ms', (report.summary.p95_ms ?? 9) <= 1],
        ['P99', `${report.summary.p99_ms?.toFixed(2)} ms`, '≤ 2 ms', (report.summary.p99_ms ?? 9) <= 2],
        ['MISSED', `${report.summary.missed_pct?.toFixed(2)}%`, '< 0.5%', (report.summary.missed_pct ?? 9) < 0.5]].map(([k, v, lim, ok]) =>
        <div key={String(k)} style={{...card, flex: 1, padding: 10, display: 'flex', flexDirection: 'column', gap: 4}}>
          <div style={{...caps, fontSize: 9}}>{k}</div><div style={{fontSize: 15, fontWeight: 800, color: ok ? C.greenText : C.red}}>{v}</div>
          <div style={{fontSize: 10, color: C.label}}>{lim}</div>
        </div>)}
    </Focusable></PanelSectionRow>}
    {report && <PanelSectionRow><div style={{fontSize: 10, color: C.muted, overflowWrap: 'anywhere'}}>Saved: {report.report}</div></PanelSectionRow>}
    <div style={gap}/>
    {running && <PanelSectionRow><div style={{...card, display: 'grid', gridTemplateColumns: '1fr auto', rowGap: 6, columnGap: 10, fontSize: 11}}>
      {[['Game frames', status?.sources], ['Generated', status?.gen], ['Repeats', status?.repeat], ['Late game frames', status?.source_late],
        ['Mode switches', status?.auto_switches], ['GPU per frame', status?.timestamps ? `${((status?.gpu_us_avg ?? 0) / 1000).toFixed(2)} ms` : '—']].map(([k, v]) =>
        <Fragment key={String(k)}><span style={{color: C.label}}>{k}</span><span style={{textAlign: 'right', fontWeight: 700}}>{v ?? 0}</span></Fragment>)}
    </div></PanelSectionRow>}
  </PanelSection>;

  const fpsColor = !generating ? C.green : (live?.fps ?? 0) >= 0.97 * hz ? C.green : (live?.fps ?? 0) >= 0.85 * hz ? C.yellow : C.red;
  const mult_text = multText(ratio);
  const gpuMs = status?.timestamps ? (status?.gpu_us_avg ?? 0) / 1000 : undefined;
  const budget = 1000 / hz;
  const [badge, badgeColor] = !running ? ['ENGINE DOWN', C.red] : status?.steam_ui ? ['STEAM MENU', C.yellow]
    : status?.state === 'degraded' ? ['PAUSED', C.yellow] : generating ? ['GENERATING', C.greenText] : ['OFF', C.label];
  const warnNote = !running || (live?.unsupported ?? 0) > 1 || status?.state === 'degraded' || note.startsWith('The game runs below');
  const space: CSSProperties = {height: 10};
  return <PanelSection>
        <PanelSectionRow><div style={{...card, display: 'flex', alignItems: 'center', gap: 16,
      background: `radial-gradient(circle at ${INSET + 54}px 50%, ${fpsColor}26 0%, transparent 62px), ${CARD_BG}`}}>
      <Ring size={108} width={8} frac={(live?.fps ?? 0) / hz} color={fpsColor}>
        <div style={{fontSize: 34, fontWeight: 900, lineHeight: 1, textShadow: `0 0 14px ${fpsColor}55`}}>{fmt(live?.fps)}</div>
        <div style={{...caps, marginTop: 4}}>FPS</div>
      </Ring>
      <div style={{display: 'flex', flexDirection: 'column', gap: 8, minWidth: 0}}>
        <div style={caps}>GAME</div>
        <div style={{fontSize: 26, fontWeight: 800, lineHeight: 1}}>{fmt(live?.real)}{generating && <span style={{color: C.green, fontSize: 20, textShadow: `0 0 10px ${C.green}66`}}> {mult_text}</span>}</div>
        <div style={{fontSize: 11, color: C.muted}}>{mode === 'cap' ? `Custom · cap ${status?.base_fps}` : mode === 'auto' ? `Auto · cap ${status?.cap_fps ?? '—'}` : running ? 'Generation off' : 'Engine not running'}</div>
        <div style={{alignSelf: 'flex-start', display: 'flex', alignItems: 'center', gap: 6, padding: '4px 10px', borderRadius: 12,
          background: C.ground, border: `1px solid ${C.line}`, color: badgeColor, fontSize: 10, fontWeight: 800, letterSpacing: '0.12em'}}>
          <span style={{width: 7, height: 7, borderRadius: 4, background: badgeColor, boxShadow: `0 0 6px ${badgeColor}`}}/>{badge}</div>
      </div>
    </div></PanelSectionRow>
    <div style={space}/>
    <PanelSectionRow><div style={{display: 'flex', gap: 8}}>
      <Mini label="PACE %" value={generating && pace !== null && pace !== undefined ? (100 * pace).toFixed(0) : '—'}
        frac={generating ? pace ?? 0 : 0} color={paceOk ? C.green : C.yellow}/>
      <Mini label="GPU MS" value={generating && gpuMs !== undefined ? gpuMs.toFixed(1) : '—'} frac={generating && gpuMs !== undefined ? gpuMs / budget : 0}
        color={(gpuMs ?? 0) < 0.6 * budget ? C.green : C.yellow}/>
      <Mini label="HZ" value={`${Math.round(hz)}`} frac={1} color={C.green}/>
    </div></PanelSectionRow>
    {note && <><div style={space}/><PanelSectionRow><div style={{...card, display: 'flex', alignItems: 'center', gap: 12, padding: `10px ${INSET}px`}}>
      <span style={{width: 34, height: 34, borderRadius: 10, flex: 'none', display: 'flex', alignItems: 'center', justifyContent: 'center',
        background: warnNote ? C.redBg : C.greenBg, color: warnNote ? C.red : C.greenText}}><Icon name={warnNote ? 'warn' : 'info'} size={18}/></span>
      <div style={{fontSize: 12, lineHeight: 1.4, color: C.text}}>{note}</div>
    </div></PanelSectionRow></>}
    <div style={space}/>
    <PanelSectionRow><Focusable style={{display: 'flex', gap: 8}} flow-children="horizontal">
      <Tile selected={mode === 'off'} disabled={busy || !running} label="Off" icon="off" onClick={() => setMode('off')}/>
      <Tile selected={mode === 'auto'} disabled={busy || !running} label="Auto" icon="auto" onClick={() => setMode('auto')}/>
      <Tile selected={mode === 'cap'} disabled={busy || !running} label={mode === 'cap' ? `Custom ${base}` : 'Custom'} icon="cap" onClick={() => setMode('cap')}/>
    </Focusable></PanelSectionRow>
    <div style={space}/>
    <PanelSectionRow><div className="gsf-green" style={{...card, padding: `2px ${INSET}px`}}>
      {mode === 'cap' && <SliderField label="Base game FPS" value={Math.min(base, hzR)} min={20} max={hzR} step={1} showValue bottomSeparator="standard"
        disabled={busy || !running} onChange={onBase}/>}
      {mode === 'auto' && !floorInSetup && <ToggleField label={`Allow ${floorMult}`} description={`Base ${capFloor} when the game cannot hold ${capLo}`} checked={x375}
        disabled={busy || !running} bottomSeparator="standard" onChange={v => setOptions(v, hdr)}/>}
      <ToggleField label="HDR games" checked={hdr} disabled={busy || !running} bottomSeparator="standard" onChange={v => setOptions(x375, v)}/>
      <ToggleField label="In-game HUD" checked={hud} disabled={busy} bottomSeparator="none"
        onChange={v => void act(async () => {setHud((await rpc.setHud(v)).hud); return v ? 'HUD on' : 'HUD off';})}/>
    </div></PanelSectionRow>
    <div style={space}/>
    <PanelSectionRow><Focusable style={{display: 'flex', gap: 8}} flow-children="horizontal">
      <Tile label="Pace" icon="pace" onClick={() => setView('pace')}/>
      <Tile label="Setup" icon="setup" dot={!!diag && !allOk} onClick={() => setView('setup')}/>
      <Tile label="Log" icon="log" onClick={() => setView('log')}/>
    </Focusable></PanelSectionRow>
    {message && <PanelSectionRow><div style={{fontSize: 10, color: C.muted, overflowWrap: 'anywhere', padding: `0 ${INSET}px`}}>{message}</div></PanelSectionRow>}
    <PanelSectionRow><div style={{fontSize: 10, color: C.label, textAlign: 'center', letterSpacing: '0.08em', paddingTop: 6}}>GSF {VERSION} · KEDAVRA</div></PanelSectionRow>
  </PanelSection>;
}

// Steam's slider and toggle are blue; turn their filled rails green (the white handles stay
// white). A filter on Steam's own classes, so it does not depend on how Steam draws the fill.
const GREEN_CONTROLS = `.gsf-green .${gamepadSliderClasses.SliderTrack}, .gsf-green .${gamepadDialogClasses.ToggleRail} {
  filter: hue-rotate(-91deg) saturate(0.6) brightness(1.25);
}`;

// Steam marks the element under the stick / D-pad cursor with .gpfocus, but our inline
// backgrounds hid its highlight. The focused tile turns light, as in GFG Extreme.
const FOCUS = `
.gsf-tile.gpfocus, .gsf-tile:focus-visible { background: linear-gradient(180deg, #ffffff 0%, #d9e4dc 100%) !important; color: #04120a !important; border-color: ${C.text} !important; box-shadow: 0 0 16px rgba(242, 246, 243, 0.35) !important; }
.gsf-tile.gsf-on.gpfocus, .gsf-tile.gsf-on:focus-visible { background: linear-gradient(180deg, #b4f7a6 0%, #7fe56f 100%) !important; box-shadow: 0 0 0 2px ${C.text}, 0 0 18px rgba(143, 240, 122, 0.5) !important; }
.gsf-btn.gpfocus, .gsf-btn:focus-visible { box-shadow: 0 0 0 2px ${C.text} !important; }
.gsf-row.gpfocus, .gsf-row:focus-visible { background: #18251c !important; border-color: ${C.green} !important; }
`;

export default definePlugin(() => ({
  name: 'GSF Kedavra',
  titleView: <div className={staticClasses.Title} style={{display: 'flex', alignItems: 'center', gap: 8}}>
    <img src={LOGO} width={26} height={26} alt=""/>
    <span style={{fontWeight: 800, letterSpacing: '0.18em'}}>GSF<span style={{color: C.green}}>·</span>KEDAVRA</span>
  </div>,
  content: <div style={{background: C.ground}}><style>{GREEN_CONTROLS + FOCUS}</style><Content/></div>,
  icon: <img src={LOGO} width={20} height={20} alt=""/>,
  onDismount() {},
}));
