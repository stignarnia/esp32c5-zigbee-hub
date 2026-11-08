'use strict';
// Zigbee Hub web UI. No framework: a hash router, one status object refreshed over
// a WebSocket every 2 s, and four pages: Devices, Automations (Lua scripts), Status
// (read-only) and Settings (everything that changes something).

const $ = (sel) => document.querySelector(sel);
const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) =>
  ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

// Light / dark: follows the system until the header toggle is used; the choice is kept in
// this browser only.
const theme = {
  dark: matchMedia('(prefers-color-scheme: dark)'),
  current() { return document.documentElement.dataset.theme || (this.dark.matches ? 'dark' : 'light'); },
  label() {
    const b = $('#theme'), dark = this.current() === 'dark';
    b.textContent = dark ? '☀' : '☾';
    b.title = dark ? 'Light theme' : 'Dark theme';
  },
  toggle() {
    const t = this.current() === 'dark' ? 'light' : 'dark';
    document.documentElement.dataset.theme = t;
    try { localStorage.setItem('theme', t); } catch (_) { /* private mode: this page view only */ }
    this.label();
  },
};
try { const t = localStorage.getItem('theme'); if (t === 'light' || t === 'dark') document.documentElement.dataset.theme = t; } catch (_) { /* none */ }
theme.label();
$('#theme').addEventListener('click', () => theme.toggle());
theme.dark.addEventListener('change', () => theme.label());

let status = null;
let ws = null;
let wsRetry = null;

// ---- API ------------------------------------------------------------------------
async function api(path, opts = {}) {
  const init = { method: opts.method || 'GET', headers: {}, credentials: 'same-origin' };
  if (init.method !== 'GET') init.headers['X-Hub'] = '1';
  if (opts.json !== undefined) {
    init.headers['Content-Type'] = 'application/json';
    init.body = JSON.stringify(opts.json);
  }
  const res = await fetch(path, init);
  const body = await res.json().catch(() => ({}));
  if (res.status === 401 && path !== '/api/login') { showLogin(); throw new Error('login required'); }
  if (!res.ok) throw new Error(body.error || `HTTP ${res.status}`);
  return body;
}

// ---- formatting -----------------------------------------------------------------
const kb = (b) => `${Math.round(b / 1024)} KB`;
function dur(s) {
  const d = Math.floor(s / 86400), h = Math.floor(s / 3600) % 24, m = Math.floor(s / 60) % 60;
  return d ? `${d}d ${h}h ${m}m` : h ? `${h}h ${m}m` : `${m}m ${s % 60}s`;
}
function rssiBadge(r) {
  const cls = r >= -60 ? 'ok' : r >= -72 ? 'warn' : 'bad';
  return `<span class="badge ${cls}">${r} dBm</span>`;
}
const dl = (rows) => `<dl>${rows.map(([k, v]) => `<dt>${esc(k).replace(/\n/g, '<br>')}</dt><dd>${v}</dd>`).join('')}</dl>`;

// ---- login / session --------------------------------------------------------------
function showLogin() {
  $('#app').hidden = true;
  $('#login').hidden = false;
  if (ws) { ws.onclose = null; ws.close(); ws = null; }
  $('#login-pw').focus();
}

$('#login-form').addEventListener('submit', async (e) => {
  e.preventDefault();
  $('#login-err').textContent = '';
  const btn = e.submitter || $('#login-form button');
  btn.disabled = true;
  try {
    await api('/api/login', { method: 'POST', json: { password: $('#login-pw').value } });
    $('#login-pw').value = '';
    await start();
  } catch (err) {
    $('#login-err').textContent = err.message;
  } finally {
    btn.disabled = false;
  }
});

$('#logout').addEventListener('click', async () => {
  try { await api('/api/logout', { method: 'POST' }); } catch (_) { /* already out */ }
  showLogin();
});

// ---- live status -------------------------------------------------------------------
function connectWs() {
  clearTimeout(wsRetry);
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(`${proto}://${location.host}/ws`);
  ws.onopen = () => $('#dot').classList.add('on');
  ws.onmessage = (ev) => {
    const msg = JSON.parse(ev.data);
    if (msg.type === 'zb') { pages.devices.apply(msg); return; }  // pushed on every device change
    status = msg;
    onStatus();
  };
  ws.onclose = () => {
    $('#dot').classList.remove('on');
    wsRetry = setTimeout(async () => {
      try { await refresh(); connectWs(); } catch (_) { wsRetry = setTimeout(connectWs, 3000); }
    }, 2000);
  };
}

async function refresh() {
  status = await api('/api/status');
  onStatus();
}

function onStatus() {
  $('#hostname').textContent = status.hostname;
  document.title = `${status.hostname} · Zigbee Hub`;
  const page = pages[currentPage()];
  if (page && page.live) page.live();
}

// ---- pages --------------------------------------------------------------------------
const pages = {};
const currentPage = () => (location.hash.slice(1) || 'devices');

function render() {
  const name = currentPage();
  document.querySelectorAll('nav a').forEach((a) =>
    a.classList.toggle('active', a.getAttribute('href') === `#${name}`));
  if (name !== 'devices') pages.devices.stop();
  if (name !== 'automations') pages.automations.stop();
  if (name !== 'settings') motionui.stop();
  (pages[name] || pages.devices).render();
}
window.addEventListener('hashchange', render);

// Devices -------------------------------------------------------------------------------
// The hub pushes a snapshot over the WebSocket within 100 ms of any change (a sensor
// opening, a device announcing); polling only while the WebSocket is down.
const ZONE_STATE = { contact: ['closed', 'OPEN'], motion: ['clear', 'MOTION'], water: ['dry', 'LEAK'],
  fire: ['ok', 'FIRE'], co: ['ok', 'CO'], vibration: ['still', 'VIBRATION'] };
pages.devices = {
  timer: null, data: null, rev: -1, reiv: {},
  stop() { clearInterval(this.timer); this.timer = null; },
  async render() {
    $('#view').innerHTML = `
      <section class="card"><h2>Zigbee network</h2><div id="zb-net" class="muted">Loading…</div>
        <div class="row"><button id="zb-scan">Scan for devices</button><span id="zb-permit" class="muted"></span></div>
      </section>
      <div id="zb-devs" class="grid"></div>
      <section class="card"><h2>Network map</h2><div id="zb-map" class="muted">—</div></section>`;
    $('#zb-scan').addEventListener('click', () => this.permit());
    await this.poll();
    this.stop();
    this.timer = setInterval(() => { if (!ws || ws.readyState !== WebSocket.OPEN) this.poll(); }, 2000);
  },
  async poll() {
    try { this.apply(await api('/api/zb')); } catch (e) { /* offline: keep the last view */ }
  },
  apply(d) {
    if (!$('#zb-net')) return;  // another page is open
    this.data = d;
    const n = d.network;
    $('#zb-net').innerHTML = n.up ? dl([['Channel', n.channel], ['PAN ID', esc(n.pan)], ['Extended PAN', `<code>${esc(n.ext_pan)}</code>`],
      ['Devices', d.devices.length]]) : 'Starting…';
    const open = n.permit_s > 0;
    $('#zb-scan').textContent = open ? 'Stop scanning' : 'Scan for devices';
    $('#zb-permit').innerHTML = open ? `<span class="badge ok">open</span> ${n.permit_s} s left — insert the battery or press the device's pairing button` : '';
    this.list(d.devices);
    this.map(n, d.devices);
  },
  map(n, devs) {
    const el = $('#zb-map');
    if (!n.up) { el.textContent = '—'; return; }
    const byIeee = Object.fromEntries(devs.map((v) => [v.ieee, v]));
    const type = ['coordinator', 'router', 'end device'];
    const rel = ['parent', 'child', 'sibling', 'none', 'previous child', 'unauthenticated child'];
    const lqiBadge = (q) => `<span class="badge ${q >= 150 ? 'ok' : q >= 80 ? 'warn' : 'bad'}">LQI ${q}</span>`;
    const rows = (n.neighbors || []).map((x) => {
      const v = byIeee[x.ieee];
      const name = v ? (v.name || [v.manuf, v.model].filter(Boolean).join(' ')) : `0x${x.short.toString(16).padStart(4, '0')}`;
      return `<tr><td>${esc(name)}</td><td>${type[x.type] || x.type}${x.rx_on ? '' : ' (sleepy)'}</td>
        <td>${rel[x.relationship] || x.relationship}</td>
        <td>${x.lqi ? lqiBadge(x.lqi) : '<span class="muted">no frame since boot</span>'}</td>
        <td>${x.rssi ? `${x.rssi} dBm` : v && v.rssi !== undefined ? `${v.rssi} dBm (last message)` : '—'}</td></tr>`;
    });
    el.innerHTML = rows.length
      ? `<p class="muted">Hub (coordinator) → direct links, from its neighbour table:</p>
         <table><tr><th>Device</th><th>Type</th><th>Link</th><th>Quality</th><th>RSSI</th></tr>${rows.join('')}</table>`
      : 'No neighbours yet.';
  },
  list(devs) {
    const el = $('#zb-devs');
    const a = document.activeElement;
    if (a && a.tagName === 'INPUT' && el.contains(a)) return;  // don't clobber typing
    el.innerHTML = devs.length ? devs.map((v) => this.card(v)).join('') : '<div class="card muted">No devices yet. Press “Scan for devices”, then power up the device.</div>';
    el.querySelectorAll('[data-rename]').forEach((b) => b.addEventListener('click', () => this.rename(b.dataset.rename)));
    el.querySelectorAll('[data-remove]').forEach((b) => b.addEventListener('click', () => this.remove(b.dataset.remove)));
    el.querySelectorAll('[data-reinterview]').forEach((b) => b.addEventListener('click', () => this.reinterview(b.dataset.reinterview)));
  },
  card(v) {
    const title = v.name || [v.manuf, v.model].filter(Boolean).join(' ') || `0x${v.short.toString(16).padStart(4, '0')}`;
    const st = !v.available ? '<span class="badge bad">offline</span>'
      : v.status === 'ready' ? '<span class="badge ok">ready</span>'
      : v.status === 'interviewing' ? '<span class="badge warn">interviewing…</span>'
      : '<span class="badge warn">waiting for it to wake up</span>';
    const rows = [];
    if (v.kind && v.zone_status !== undefined) {
      const [idle, alarm] = ZONE_STATE[v.kind] || ['normal', 'ALARM'];
      rows.push([v.kind, v.alarm ? `<span class="badge bad">${alarm}</span>` : `<span class="badge ok">${idle}</span>`]);
      if (v.tamper) rows.push(['Tamper', '<span class="badge bad">tampered</span>']);
    } else if (v.kind) rows.push([v.kind, '<span class="muted">no report yet</span>']);
    if (v.temperature !== undefined) rows.push(['Temperature', `${v.temperature.toFixed(1)} °C`]);
    if (v.humidity !== undefined) rows.push(['Humidity', `${v.humidity.toFixed(0)} %`]);
    if (v.on !== undefined) rows.push(['State', v.on ? 'on' : 'off']);
    if (v.occupied !== undefined) rows.push(['Occupancy', v.occupied ? 'occupied' : 'clear']);
    if (v.battery !== undefined) rows.push(['Battery', `${Math.round(v.battery)} %${v.battery_low ? ' <span class="badge bad">low</span>' : ''}`]);
    else if (v.battery_v !== undefined) rows.push(['Battery', `${v.battery_v.toFixed(1)} V`]);
    if (v.rssi !== undefined) rows.push(['Signal', rssiBadge(v.rssi)]);
    rows.push(['Last seen', v.seen_s < 0 ? 'not since boot' : v.seen_s < 5 ? 'just now' : `${dur(v.seen_s)} ago`]);
    rows.push(['Offline after', `${dur(v.timeout_s)} silent${v.sleepy ? '' : ' + 3 pings'}`]);
    rows.push(['Model', esc([v.manuf, v.model].filter(Boolean).join(' / ') || '—')]);
    rows.push(['IEEE', `<code>${esc(v.ieee)}</code>`]);
    rows.push(['Type', v.sleepy ? 'battery (sleepy)' : 'mains / router']);
    // Re-interview feedback: requested -> running -> done (shown 15 s).
    const r = this.reiv[v.ieee];
    let note = '';
    if (r) {
      if (v.status !== 'ready') r.busy = true;
      if (r.busy && v.status === 'ready' && !r.done) r.done = Date.now();
      if (r.done && Date.now() - r.done > 15000) delete this.reiv[v.ieee];
      else if (r.done) note = '<p class="note ok">Re-interview done.</p>';
      else if (r.busy && v.status === 'interviewing') note = '<p class="note warn">Re-interviewing…</p>';
      else note = `<p class="note warn">Re-interview requested${v.sleepy ? ' — it starts when the sensor next wakes up: open and close it, or press its button' : '…'}</p>`;
    }
    const pending = r && !r.done && this.reiv[v.ieee];
    const clusters = v.eps.map((e) => `ep ${e.id}: ${e.in.map((c) => c.toString(16).padStart(4, '0')).join(' ')}`).join('<br>');
    return `<section class="card"><h2>${esc(title)} ${st}</h2>${dl(rows)}
      ${note}<details><summary class="muted">Clusters</summary><p class="muted"><code>${clusters || '—'}</code></p></details>
      <label>Name <input type="text" id="name-${v.ieee}" value="${esc(v.name)}" maxlength="31" placeholder="e.g. Front door"></label>
      <div class="row"><button class="ghost" data-rename="${v.ieee}">Save name</button>
        <button class="ghost" data-reinterview="${v.ieee}"${pending ? ' disabled' : ''} title="Read its endpoints and clusters again and redo enrollment and reporting${v.sleepy ? '. Battery devices: starts when it next wakes up (open/close it or press its button)' : ''}">${pending ? 'Re-interview requested' : 'Re-interview'}</button>
        <button class="danger" data-remove="${v.ieee}">Remove</button></div></section>`;
  },
  async permit() {
    const open = this.data && this.data.network.permit_s > 0;
    try { await api('/api/zb/permit', { method: 'POST', json: { seconds: open ? 0 : 120 } }); } catch (e) { alert(e.message); }
    this.poll();
  },
  async rename(ieee) {
    const name = $(`#name-${ieee}`).value.trim();
    try { await api('/api/zb/rename', { method: 'POST', json: { ieee, name } }); document.activeElement.blur(); } catch (e) { alert(e.message); }
    this.poll();
  },
  async reinterview(ieee) {
    if (document.activeElement) document.activeElement.blur();
    try { await api('/api/zb/reinterview', { method: 'POST', json: { ieee } }); } catch (e) { alert(`Re-interview failed: ${e.message}`); return; }
    this.reiv[ieee] = { busy: false, done: 0 };
    this.poll();
  },
  async remove(ieee) {
    if (!confirm('Remove this device? It is asked to leave the network; to use it again, pair it again.')) return;
    try { await api('/api/zb/remove', { method: 'POST', json: { ieee } }); } catch (e) { alert(e.message); }
    this.poll();
  },
};

// Automations ----------------------------------------------------------------------------
// Lua scripts stored on the hub: list with live status, an editor (syntax-checked on save),
// the script log, and the state of the GPIO pins scripts use.
// The example scripts: examples/*.lua at the top of the repo, put here at build time by
// components/web/gen_app_js.py (it replaces the marker). Offered in the editor only.
const EXAMPLES = /*@EXAMPLES@*/ {};

// Script editor ------------------------------------------------------------------------
// Both editors give the same handle: get(), set(text), selectLine(n), focus().

// Textarea with a line-number gutter: shown at once, and kept if cm.js can't load.
function textEditor(box, src) {
  box.innerHTML = '<pre aria-hidden="true"></pre><textarea spellcheck="false" autocapitalize="off" autocomplete="off" wrap="off"></textarea>';
  const gut = box.querySelector('pre'), ta = box.querySelector('textarea');
  const gutter = () => {
    gut.textContent = Array.from({ length: ta.value.split('\n').length }, (_, i) => i + 1).join('\n');
    gut.scrollTop = ta.scrollTop;
  };
  ta.value = src;
  ta.addEventListener('input', gutter);
  ta.addEventListener('scroll', () => { gut.scrollTop = ta.scrollTop; });
  ta.addEventListener('keydown', (e) => {
    if (e.key === 'Tab' && !e.shiftKey) {
      e.preventDefault();
      ta.setRangeText('  ', ta.selectionStart, ta.selectionEnd, 'end');
      gutter();
    }
  });
  gutter();
  return {
    get: () => ta.value,
    set(text) { ta.value = text; gutter(); },
    selectLine(line) {
      const lines = ta.value.split('\n');
      let start = 0;
      for (let i = 0; i < line - 1 && i < lines.length; i++) start += lines[i].length + 1;
      ta.focus();
      ta.setSelectionRange(start, start + (lines[line - 1] || '').length);
      ta.scrollTop = Math.max(0, (line - 5) * parseFloat(getComputedStyle(ta).lineHeight));
    },
    focus: () => ta.focus(),
  };
}

// CodeMirror (syntax highlighting, search, completion of the hub's API), loaded only
// when the editor is first opened. Resolves false if it can't be loaded.
let cmLoading = null;
function loadCodeMirror() {
  if (window.HubEditor) return Promise.resolve(true);
  if (!cmLoading) {
    cmLoading = new Promise((resolve) => {
      const el = document.createElement('script');
      el.src = '/cm.js';
      el.onload = () => resolve(!!window.HubEditor);
      el.onerror = () => { cmLoading = null; el.remove(); resolve(false); };
      document.head.appendChild(el);
    });
  }
  return cmLoading;
}

pages.automations = {
  timer: null, logTimer: null, data: null, editing: null, logLast: 0, logLines: [],
  stop() { clearInterval(this.timer); clearInterval(this.logTimer); this.timer = this.logTimer = null; },
  async render() {
    $('#view').innerHTML = `
      <div id="au-safe"></div>
      <div class="grid">
        <section class="card"><h2>Scripts</h2><div id="au-clock" class="muted"></div><div id="au-list" class="muted">Loading…</div>
          <div class="row"><button id="au-new">New script</button></div></section>
        <section class="card" id="au-pins-card"><h2>Pins</h2><div id="au-pins" class="muted">—</div></section>
        <section class="card" id="au-secrets"><h2>Secrets</h2></section>
      </div>
      <section class="card" id="au-editor" hidden></section>
      <div class="grid" id="auto-settings"></div>
      <section class="card"><h2>Script log</h2>
        <div class="row"><select id="au-filter"><option value="">All scripts</option></select>
          <button class="ghost" id="au-clear">Clear view</button></div>
        <pre id="au-log" class="log"></pre></section>`;
    autoset.render();
    $('#au-new').addEventListener('click', () => this.edit(null));
    $('#au-clear').addEventListener('click', () => { this.logLines = []; this.drawLog(); });
    $('#au-filter').addEventListener('change', () => this.drawLog());
    this.logLast = 0; this.logLines = []; this.editing = null;
    await this.poll();
    await this.pollLog();
    this.stop();
    this.timer = setInterval(() => this.poll(), 3000);
    this.logTimer = setInterval(() => this.pollLog(), 1500);
  },
  async poll() {
    try { this.data = await api('/api/auto'); } catch (e) { return; }
    if (!$('#au-list')) { this.stop(); return; }
    this.drawList();
    this.drawPins();
    const sel = $('#au-filter');
    const have = new Set([...sel.options].map((o) => o.value));
    this.data.scripts.forEach((s) => { if (!have.has(s.name)) sel.add(new Option(s.name, s.name)); });
  },
  drawSafe() {
    const b = this.data.boot || {};
    const el = $('#au-safe');
    if (!b.safe_mode) { el.innerHTML = ''; return; }
    el.innerHTML = `<div class="note bad"><b>Safe mode:</b> the hub reset ${b.abnormal_resets || 3} times in a row
      soon after starting (last: ${esc(b.last_reset)}), so scripts were not started. Zigbee and everything
      else run normally. <button id="au-safe-go">Start scripts</button></div>`;
    $('#au-safe-go').addEventListener('click', async () => {
      try { await api('/api/auto/safe', { method: 'POST', json: {} }); this.poll(); } catch (e) { alert(e.message); }
    });
  },
  drawList() {
    const d = this.data;
    this.drawSafe();
    $('#au-clock').innerHTML = d.clock_set ? `Clock: ${esc(d.local_time)}`
      : '<span class="badge warn">clock not set</span> cron jobs wait until the hub gets the time from the network';
    const el = $('#au-list');
    if (!d.scripts.length) { el.innerHTML = '<p class="muted">No scripts yet.</p>'; return; }
    const badge = (s) => !s.enabled ? '<span class="badge">off</span>'
      : s.status === 'running' ? '<span class="badge ok">running</span>'
      : s.status === 'error' ? '<span class="badge bad">stopped by an error</span>' : '<span class="badge warn">stopped</span>';
    const meta = (s) => [s.status === 'running' ? kb(s.mem) : '', `${s.calls} run(s), ${s.busy_ms} ms`,
      s.timers ? `${s.timers} timer(s)` : '', s.cron ? `${s.cron} cron` : ''].filter(Boolean).join(' · ');
    el.innerHTML = d.scripts.map((s) => `
      <div class="srow">
        <div class="srow-main"><b>${esc(s.name)}</b> ${badge(s)}<div class="muted small">${meta(s)}</div>
          ${s.error ? `<div class="err small">${esc(s.error)}</div>` : ''}</div>
        <div class="srow-actions"><label class="switch"><input type="checkbox" data-en="${esc(s.name)}" ${s.enabled ? 'checked' : ''}> on</label>
          <button class="ghost" data-edit="${esc(s.name)}">Edit</button>
          ${s.status === 'error' && s.enabled ? `<button class="ghost" data-restart="${esc(s.name)}">Restart</button>` : ''}
          <button class="danger" data-del="${esc(s.name)}">Delete</button></div>
      </div>`).join('');
    el.querySelectorAll('[data-en]').forEach((c) => c.addEventListener('change', () => this.enable(c.dataset.en, c.checked)));
    el.querySelectorAll('[data-edit]').forEach((b) => b.addEventListener('click', () => this.edit(b.dataset.edit)));
    el.querySelectorAll('[data-restart]').forEach((b) => b.addEventListener('click', () => this.act('/api/auto/restart', b.dataset.restart)));
    el.querySelectorAll('[data-del]').forEach((b) => b.addEventListener('click', () => {
      if (confirm(`Delete the script "${b.dataset.del}" and its saved state?`)) this.act('/api/auto/delete', b.dataset.del);
    }));
  },
  drawPins() {
    const pins = this.data.gpio;
    $('#au-pins').innerHTML = pins.length ? `<table><tr><th>Pin</th><th>Mode</th><th>Level</th><th>Used by</th></tr>${pins.map((p) => `
      <tr><td>${esc(p.name || '—')} <span class="muted">GPIO ${p.pin}</span></td><td>${esc(p.mode)}</td>
        <td>${p.playing ? '<span class="badge warn">playing</span>' : p.level ? '<span class="badge ok">high</span>' : 'low'}</td>
        <td>${esc(p.owner || '—')}</td></tr>`).join('')}</table>`
      : 'No pins enabled yet (GPIO, below).';
    const led = this.data.led;
    if (led) {
      const [r, g, b] = led.rgb;
      const on = r || g || b;
      $('#au-pins').insertAdjacentHTML('beforeend', `<p class="small">RGB LED (GPIO 27):
        ${on ? `<span class="swatch" style="background: rgb(${r}, ${g}, ${b})"></span> ${r}, ${g}, ${b}` : 'off'}
        ${led.pending ? '<span class="badge warn">waiting for a buzzer to finish</span>' : ''}
        · used by ${esc(led.owner || '—')}</p>`);
    }
  },
  async enable(name, on) {
    try { await api('/api/auto/enable', { method: 'POST', json: { name, enabled: on } }); } catch (e) { alert(e.message); }
    this.poll();
  },
  async act(path, name) {
    try { await api(path, { method: 'POST', json: { name } }); } catch (e) { alert(e.message); }
    if (path.endsWith('delete') && this.editing === name) this.closeEditor();
    this.poll();
  },
  async edit(name) {
    let src = EXAMPLES.Template;
    if (name) {
      try {
        const r = await fetch(`/api/auto/script?name=${encodeURIComponent(name)}`, { credentials: 'same-origin' });
        if (!r.ok) throw new Error(`HTTP ${r.status}`);
        src = await r.text();
      } catch (e) { alert(e.message); return; }
    }
    this.editing = name;
    const s = name && this.data.scripts.find((x) => x.name === name);
    const ed = $('#au-editor');
    ed.hidden = false;
    ed.innerHTML = `<h2>${name ? `Edit <code>${esc(name)}</code>` : 'New script'}</h2>
      <div class="row">${name ? '' : '<label class="inline">Name <input type="text" id="au-name" maxlength="24" placeholder="door-alarm" pattern="[a-z0-9_-]+"></label>'}
        <label class="switch"><input type="checkbox" id="au-enabled" ${!s || s.enabled ? 'checked' : ''}> enabled</label>
        <select id="au-example"><option value="">Insert an example…</option>${Object.keys(EXAMPLES).map((k) => `<option>${esc(k)}</option>`).join('')}</select></div>
      <div class="editor" id="au-box"></div>
      <p id="au-msg"></p>
      <div class="row"><button id="au-save">Save</button><button class="ghost" id="au-cancel">Close</button>
        <span class="muted">Saving checks the syntax first; nothing is stored if it fails.</span></div>`;
    // Plain textarea at once; CodeMirror (cm.js, ~120 KB, cached) replaces it when loaded.
    const session = this.session = (this.session || 0) + 1;
    this.ed = textEditor($('#au-box'), src);
    loadCodeMirror().then((ok) => {
      if (!ok || this.session !== session || !$('#au-box')) return;
      const text = this.ed.get();
      $('#au-box').textContent = '';
      this.ed = window.HubEditor.create($('#au-box'), text);
    });
    $('#au-example').addEventListener('change', (e) => {
      const k = e.target.value, cur = this.ed.get();
      if (k && (cur.trim() === '' || cur === EXAMPLES.Template || confirm('Replace the editor content with the example?'))) {
        this.ed.set(EXAMPLES[k]);
      }
      e.target.value = '';
    });
    $('#au-save').addEventListener('click', () => this.save());
    $('#au-cancel').addEventListener('click', () => this.closeEditor());
    ed.scrollIntoView({ behavior: 'smooth' });
  },
  closeEditor() {
    this.editing = null;
    this.session = (this.session || 0) + 1;
    if (this.ed && this.ed.destroy) this.ed.destroy();
    this.ed = null;
    const ed = $('#au-editor');
    if (ed) { ed.hidden = true; ed.innerHTML = ''; }
  },
  async save() {
    const name = this.editing || $('#au-name').value.trim();
    const msg = $('#au-msg');
    if (!/^[a-z0-9_-]{1,24}$/.test(name)) { msg.className = 'err'; msg.textContent = 'Name: 1–24 characters, a–z 0–9 _ -'; return; }
    const ed = this.ed;
    const en = $('#au-enabled').checked ? 1 : 0;
    msg.className = 'muted'; msg.textContent = 'Saving…';
    try {
      const r = await fetch(`/api/auto/script?name=${encodeURIComponent(name)}&enabled=${en}`, {
        method: 'POST', credentials: 'same-origin', headers: { 'X-Hub': '1', 'Content-Type': 'text/plain' }, body: ed.get() });
      const body = await r.json().catch(() => ({}));
      if (r.status === 401) { showLogin(); return; }
      if (!r.ok) {
        msg.className = 'err';
        msg.textContent = body.error || `HTTP ${r.status}`;
        if (body.line) ed.selectLine(body.line);
        return;
      }
      this.editing = name;
      if (body.started === false) {
        msg.className = 'err';
        msg.textContent = `Saved, but it stopped while starting: ${body.error.split('\n')[0]}`;
        if (body.line) ed.selectLine(body.line);
      } else {
        msg.className = 'good'; msg.textContent = en ? 'Saved and started (see the log below).' : 'Saved (disabled).';
      }
      this.poll();
    } catch (e) { msg.className = 'err'; msg.textContent = e.message; }
  },
  async pollLog() {
    let d;
    try { d = await api(`/api/auto/log?after=${this.logLast}`); } catch (e) { return; }
    if (!$('#au-log')) return;
    if (d.lines.length) {
      this.logLines.push(...d.lines);
      if (this.logLines.length > 500) this.logLines.splice(0, this.logLines.length - 500);
      this.drawLog();
    }
    this.logLast = d.last;
  },
  drawLog() {
    const f = $('#au-filter').value;
    const el = $('#au-log');
    const stick = el.scrollTop + el.clientHeight >= el.scrollHeight - 20;
    const t = (s) => (s >= 0 ? new Date(s * 1000).toLocaleTimeString() : `+${-s}s`);
    el.innerHTML = this.logLines.filter((l) => !f || l.script === f).map((l) =>
      `<span class="${l.level === 'E' ? 'err' : ''}">${esc(t(l.t))} ${esc(l.script)}: ${esc(l.msg)}</span>`).join('\n') || '<span class="muted">Nothing yet.</span>';
    if (stick) el.scrollTop = el.scrollHeight;
  },
};

// Secrets and GPIO (Automations page) and time zone (Settings page) ---------------------
const TZ_PRESETS = [
  ['UTC', 'UTC0'], ['Central Europe (Rome, Berlin, Paris)', 'CET-1CEST,M3.5.0,M10.5.0/3'],
  ['UK, Ireland, Portugal', 'GMT0BST,M3.5.0/1,M10.5.0'], ['Eastern Europe (Athens, Helsinki)', 'EET-2EEST,M3.5.0/3,M10.5.0/4'],
  ['US Eastern', 'EST5EDT,M3.2.0,M11.1.0'], ['US Central', 'CST6CDT,M3.2.0,M11.1.0'],
  ['US Mountain', 'MST7MDT,M3.2.0,M11.1.0'], ['US Pacific', 'PST8PDT,M3.2.0,M11.1.0'],
];
const GPIO_PINS = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 23, 24, 25, 26, 27];
const GPIO_GROUND_ONLY = [27];  // strapping pin + RGB LED: buzzer ground only
const GPIO_MODES = [['out', 'output'], ['in', 'input'], ['in_pullup', 'input, pull-up'], ['in_pulldown', 'input, pull-down'], ['ground', 'ground (always low)']];

const autoset = {
  data: null,
  // Secrets (#au-secrets) + GPIO (#auto-settings) live on the Automations page, the time zone on the
  // Settings page (#tz-settings); each page has one of the two areas.
  async render() {
    const area = $('#auto-settings'), tzArea = $('#tz-settings');
    if (!area && !tzArea) return;
    try { this.data = await api('/api/auto'); } catch (e) { (area || tzArea).innerHTML = `<div class="card err">${esc(e.message)}</div>`; return; }
    const d = this.data;
    if (tzArea) this.renderTz(tzArea, d);
    if (!area) return;
    $('#au-secrets').innerHTML = `<h2>Secrets</h2>
        <p class="muted">Values scripts read with <code>secret("name")</code>, e.g. a URL with a token. Write-only:
        they never leave the hub, and the script log masks them.</p>
        ${d.secrets.length ? `<table>${d.secrets.map((n) => `<tr><td><code>${esc(n)}</code></td>
          <td class="actions"><button class="danger" data-sdel="${esc(n)}">Delete</button></td></tr>`).join('')}</table>` : '<p class="muted">None yet.</p>'}
        <label>Name <input type="text" id="sec-name" maxlength="15" placeholder="alexa_alarm_url" autocomplete="off"></label>
        <label>Value <input type="password" id="sec-val" maxlength="512" autocomplete="new-password" placeholder="saved as is; replaces the old value"></label>
        <div class="row"><button id="sec-save">Save secret</button><span id="sec-msg"></span></div>`;
    area.innerHTML = `
      <section class="card wide"><h2>GPIO</h2>
        <p class="muted">Pins scripts may use, by name or number. Outputs start low, drive strength 3; a
        stopped script's outputs go low. “Ground” holds a pin low for a buzzer's second leg; say which
        output it's the ground for. GPIO 27 (the RGB LED's pin) can only be a ground.</p>
        <div id="gp-table"><div class="gp gp-head"><span>Name</span><span>Pin</span><span>Mode</span><span></span></div></div>
        <div class="row"><button class="ghost" id="gp-add">Add pin</button><button id="gp-save">Save pins</button><span id="gp-msg"></span></div>
      </section>`;
    this.rows = d.gpio.map((p) => ({ name: p.name, pin: p.pin, mode: p.mode, for: p.for || '' }));
    this.drawRows();
    $('#gp-add').addEventListener('click', () => { this.readRows(); this.rows.push({ name: '', pin: GPIO_PINS.find((p) => !this.rows.some((r) => r.pin === p)) || 3, mode: 'out' }); this.drawRows(); });
    $('#gp-save').addEventListener('click', () => this.saveGpio());
    $('#sec-save').addEventListener('click', () => this.saveSecret());
    $('#au-secrets').querySelectorAll('[data-sdel]').forEach((b) => b.addEventListener('click', () => this.delSecret(b.dataset.sdel)));
  },
  renderTz(tzArea, d) {
    const preset = TZ_PRESETS.find((p) => p[1] === d.tz);
    tzArea.innerHTML = `
      <section class="card"><h2>Time zone</h2>
        <p class="muted">For <code>cron</code> and <code>os.date</code>. ${d.clock_set ? `Local time now: ${esc(d.local_time)}` : 'The clock is not set yet (it comes from the network).'}</p>
        <label>Preset <select id="tz-preset"><option value="">Custom…</option>${TZ_PRESETS.map(([n, v]) => `<option value="${esc(v)}" ${preset && preset[1] === v ? 'selected' : ''}>${esc(n)}</option>`).join('')}</select></label>
        <label>POSIX TZ string <input type="text" id="tz-val" value="${esc(d.tz)}" maxlength="63"></label>
        <div class="row"><button id="tz-save">Save time zone</button><span id="tz-msg"></span></div>
      </section>`;
    $('#tz-preset').addEventListener('change', (e) => { if (e.target.value) $('#tz-val').value = e.target.value; });
    $('#tz-save').addEventListener('click', () => this.saveTz());
  },
  drawRows() {
    const t = $('#gp-table');
    t.querySelectorAll('.gp.row-pin').forEach((r) => r.remove());
    this.rows.forEach((r, i) => {
      const row = document.createElement('div');
      row.className = 'gp row-pin';
      row.innerHTML = `<input type="text" class="gp-name" data-k="name" value="${esc(r.name)}" maxlength="15" placeholder="name, e.g. buzzer1" aria-label="Name">
        <select data-k="pin" aria-label="Pin">${GPIO_PINS.map((p) => `<option value="${p}" ${p === r.pin ? 'selected' : ''}>GPIO ${p}${GPIO_GROUND_ONLY.includes(p) ? ' (ground only)' : ''}</option>`).join('')}</select>
        <select data-k="mode" aria-label="Mode">${(GPIO_GROUND_ONLY.includes(r.pin) ? GPIO_MODES.filter(([v]) => v === 'ground') : GPIO_MODES).map(([v, n]) => `<option value="${v}" ${v === r.mode ? 'selected' : ''}>${n}</option>`).join('')}</select>
        <button class="danger" data-rm="${i}" aria-label="Remove">×</button>
        ${r.mode === 'ground' ? `<label class="gp-for">Ground for <select data-k="for" aria-label="Ground for">
          <option value="">— (not said: treated as possibly any output)</option>
          ${this.rows.filter((o) => o.mode === 'out' && o.name).map((o) => `<option value="${esc(o.name)}" ${o.name === r.for ? 'selected' : ''}>${esc(o.name)} (GPIO ${o.pin})</option>`).join('')}
        </select></label>` : ''}`;
      t.appendChild(row);
    });
    t.querySelectorAll('[data-rm]').forEach((b) => b.addEventListener('click', () => { this.readRows(); this.rows.splice(+b.dataset.rm, 1); this.drawRows(); }));
    // Switching to a ground-only pin leaves only the "ground" mode: redraw the row's modes.
    // Pin, mode or a name changed: redraw (ground-only pins, the "ground for" choices).
    t.querySelectorAll('[data-k=pin], [data-k=mode], [data-k=name]').forEach((el) => el.addEventListener('change', () => {
      this.readRows();
      this.rows.forEach((r) => { if (GPIO_GROUND_ONLY.includes(r.pin)) r.mode = 'ground'; });
      this.drawRows();
    }));
  },
  readRows() {
    this.rows = [...$('#gp-table').querySelectorAll('.gp.row-pin')].map((tr) => ({
      name: tr.querySelector('[data-k=name]').value.trim(),
      pin: +tr.querySelector('[data-k=pin]').value,
      mode: tr.querySelector('[data-k=mode]').value,
      for: tr.querySelector('[data-k=for]') ? tr.querySelector('[data-k=for]').value : '',
    }));
  },
  async post(path, json, msgSel) {
    const m = $(msgSel);
    try { await api(path, { method: 'POST', json }); m.className = 'good'; m.textContent = 'Saved.'; return true; }
    catch (e) { m.className = 'err'; m.textContent = e.message; return false; }
  },
  async saveGpio() { this.readRows(); if (await this.post('/api/auto/gpio', { pins: this.rows }, '#gp-msg')) setTimeout(() => this.render(), 800); },
  async saveSecret() {
    const name = $('#sec-name').value.trim(), value = $('#sec-val').value;
    if (!value) { $('#sec-msg').className = 'err'; $('#sec-msg').textContent = 'Value required'; return; }
    if (await this.post('/api/auto/secret', { name, value }, '#sec-msg')) setTimeout(() => this.render(), 800);
  },
  async delSecret(name) {
    if (!confirm(`Delete the secret "${name}"? Scripts reading it get nil.`)) return;
    try { await api('/api/auto/secret', { method: 'POST', json: { name, value: null } }); } catch (e) { alert(e.message); }
    this.render();
  },
  async saveTz() { if (await this.post('/api/auto/tz', { tz: $('#tz-val').value.trim() }, '#tz-msg')) setTimeout(() => this.render(), 800); },
};

// System ------------------------------------------------------------------------------
// Wi-Fi disconnect reasons in plain words (the hub sends the driver's name for the code).
const REASON_TEXT = {
  AUTH_EXPIRE: "the router didn't answer the login in 4 s (normal after an abrupt restart: it still held the old session)",
  NO_AP_FOUND: 'router not found',
  BEACON_TIMEOUT: "lost the router's signal",
  AUTH_FAIL: 'login refused (wrong password?)',
  ASSOC_FAIL: 'the router refused the connection',
  HANDSHAKE_TIMEOUT: 'password handshake timed out',
  '4WAY_HANDSHAKE_TIMEOUT': 'password handshake timed out (wrong password?)',
  ASSOC_LEAVE: 'the hub disconnected on purpose (restart or network switch)',
  CONNECTION_FAIL: 'connection failed',
};

pages.status = {
  render() { $('#view').innerHTML = '<div class="grid" id="sys"></div>'; tsui.peers = null; this.live(); tsui.loadPeers(); },
  live() {
    const el = $('#sys');
    if (!el || !status) return;
    const s = status, w = s.wifi, m = s.memory, f = s.firmware, u = f.last_update;
    const intTotal = 320 * 1024;
    const last = u.result === 'updated' ? `<span class="badge ok">installed</span> ${esc(u.to_version)} <code>${esc(u.to.slice(0, 8))}</code>`
      : u.result === 'trial' ? `<span class="badge">confirming</span> ${esc(u.to_version)} <code>${esc(u.to.slice(0, 8))}</code>`
      : u.result === 'rolled_back' ? `<span class="badge bad">rolled back</span> <code>${esc(u.to.slice(0, 8))}</code> failed`
      : '<span class="muted">none yet</span>';
    el.innerHTML = `
      <section class="card"><h2>Wi-Fi</h2>${dl([
        ['Status', w.connected ? '<span class="badge ok">connected</span>' : '<span class="badge bad">down</span>'],
        ['Network', `${esc(w.ssid)} <span class="muted">(${w.network})</span>`],
        ['Channel', `${w.channel || '–'} ${w.dfs ? '<span class="badge warn">DFS</span>' : ''} <span class="muted">5 GHz</span>`],
        ['Signal', w.connected ? rssiBadge(w.rssi) : '–'],
        ['IP', esc(w.ip)],
        ['MAC', esc(w.mac)],
        ['Online after boot', w.first_ip_ms >= 0 ? `${(w.first_ip_ms / 1000).toFixed(1)} s` : '–'],
      ])}</section>
      <section class="card"><h2>Wi-Fi recovery</h2>${dl([
        ['Connects', w.connects],
        ['Disconnects', `${w.disconnects}${w.last_reason ? `<br><span class="muted">last: ${esc(REASON_TEXT[w.last_reason_name] || w.last_reason_name || 'code')} (${w.last_reason})</span>` : ''}`],
        ['AP not found (201)', w.no_ap_found],
        ['Network failovers', w.failovers],
        ['Driver restarts', w.driver_restarts],
        ['Watchdog reboots', w.watchdog_reboots],
      ])}</section>
      ${tsui.statusCard(s.tailscale)}
      <section class="card"><h2>Memory</h2>
        <div class="muted">Internal RAM: ${kb(m.internal_free)} free, lowest ${kb(m.internal_min)}</div>
        <div class="bar"><i style="width:${Math.min(100, 100 - (m.internal_free / intTotal) * 100).toFixed(0)}%"></i></div>
        <div class="muted">PSRAM: ${kb(m.psram_free)} free</div>
        <div class="bar"><i style="width:${(100 - (m.psram_free / (8192 * 1024)) * 100).toFixed(1)}%"></i></div>
        ${dl([['Largest internal block', kb(m.internal_largest)]])}
      </section>
      <section class="card"><h2>System</h2>${dl([
        ['Uptime', dur(s.uptime_s)],
        ['Last reset', esc(s.boot.reset_reason) + (s.boot.planned_reason ? `<br><span class="muted">${esc(s.boot.planned_reason)}</span>` : '')],
      ])}</section>
      <section class="card"><h2>Firmware</h2>${dl([
        ['Version', `${esc(f.version)} <span class="muted">(git)</span>`],
        ['Build', `<code>${esc(f.build_id.slice(0, 8))}</code>`],
        ['Built', esc(f.built)],
        ['ESP-IDF', esc(f.idf)],
        ['Slot', esc(f.partition)],
        ['State', f.on_trial ? '<span class="badge warn">on trial</span>' : '<span class="badge ok">confirmed</span>'],
        ['Last over-the-air\nupdate', last + (u.result === 'updated' && u.to !== f.build_id
          ? '<br><span class="muted">flashed over USB since</span>' : '')],
      ])}</section>`;
  },
};

// Settings: everything that changes something. Wi-Fi networks, Tailscale, firmware update,
// restart, diagnostic log download, and the time zone (secrets and GPIO are on Automations).
pages.settings = {
  render() {
    $('#view').innerHTML = `
      <div id="wifi-note"></div>
      <div id="wifi-area"><div class="card">Loading…</div></div>
      <div class="grid">
        <section class="card" id="ts-card"><h2>Tailscale</h2><div class="muted">Loading…</div></section>
        <section class="card"><h2>Firmware update</h2>
          <p class="muted">Upload <code>build/esp32c5-zigbee-hub.bin</code>, or run <code>./flash.sh --ota</code>.
          The new firmware runs on trial and the hub goes back to this version by itself if it fails.</p>
          <input type="file" id="fw-file" accept=".bin">
          <div class="row"><button id="fw-upload" disabled>Upload &amp; install</button></div>
          <div id="fw-progress" class="row" hidden><progress id="fw-bar" max="100" value="0"></progress></div>
          <p id="fw-msg"></p>
        </section>
        <section class="card"><h2>Diagnostic log</h2>
          <p class="muted">Warnings, errors, Wi-Fi events, updates and reboots, plus a sample every
          5 minutes, kept on the hub (the last ~500 KB) with UTC timestamps.</p>
          <div class="row"><a class="button" href="/api/logs" download>Download log</a></div>
        </section>
        <section class="card"><h2>Restart</h2>
          <div class="row"><button class="danger" id="reboot">Restart the hub</button></div>
        </section>
        <section class="card" id="mo-card"><h2>Motion</h2><div class="muted">Loading…</div></section>
      </div>
      <div class="grid" id="tz-settings"></div>`;
    $('#fw-file').addEventListener('change', () => { $('#fw-upload').disabled = !$('#fw-file').files.length; });
    $('#fw-upload').addEventListener('click', () => fw.upload());
    $('#reboot').addEventListener('click', async () => {
      if (!confirm('Restart the hub now?')) return;
      try { await api('/api/reboot', { method: 'POST' }); fw.msg('Restarting…'); fw.waitBack(null); }
      catch (e) { fw.msg(esc(e.message), 'err'); }
    });
    wifi.render();
    tsui.renderSettings();
    autoset.render();
    motionui.render();
  },
  live() { wifi.live(); tsui.liveSettings(); },
};

// Motion (part of Settings) -----------------------------------------------------------
// Wi-Fi motion sensing. The two buttons are the calibration: each one learns what that
// state looks like (stay still 30 s / keep moving 15 s).
const motionui = {
  timer: null,
  stop() { clearInterval(this.timer); this.timer = null; },
  async render() {
    const card = $('#mo-card');
    if (!card) return;
    card.innerHTML = `<h2>Motion</h2>
      <div id="mo-state" style="font-size:2.5rem;font-weight:700;text-align:center;margin:.5rem 0">…</div>
      <p id="mo-note" class="muted" style="text-align:center"></p>
      <div class="row" style="justify-content:center"><button id="mo-still">Not moving</button><button id="mo-move">Moving</button></div>`;
    $('#mo-still').addEventListener('click', () => this.learn(false));
    $('#mo-move').addEventListener('click', () => this.learn(true));
    await this.poll();
    this.stop();
    this.timer = setInterval(() => this.poll(), 1000);
  },
  async learn(moving) {
    try { await api('/api/motion/learn', { method: 'POST', json: { moving } }); await this.poll(); }
    catch (e) { $('#mo-note').textContent = e.message; }
  },
  async poll() {
    if (!$('#mo-card')) { this.stop(); return; }
    let d;
    try { d = await api('/api/motion'); } catch (_) { return; }
    $('#mo-state').textContent = !d.sensing ? '—' : d.moving ? 'Moving' : 'Not moving';
    $('#mo-note').textContent = !d.sensing ? 'No Wi-Fi signal: not sensing'
      : d.learning === 'still' ? 'Learning: stay still for 30 s…'
      : d.learning === 'moving' ? 'Learning: keep moving for 15 s…' : '';
  },
};

// Wi-Fi networks (part of Settings) ---------------------------------------------------
const wifi = {
  data: null,
  async render() {
    const area = $('#wifi-area');
    if (!area) return;
    try { this.data = await api('/api/wifi'); } catch (e) { area.innerHTML = `<div class="card err">${esc(e.message)}</div>`; return; }
    const d = this.data;
    const netForm = (n, i) => `
      <section class="card"><h2>${i === 0 ? 'Primary Wi-Fi network' : 'Backup Wi-Fi network'}
        ${d.current === i && d.connected ? '<span class="badge ok">in use</span>' : ''}</h2>
        <label>SSID <input type="text" id="ssid${i}" value="${esc(n.ssid)}" maxlength="32" autocomplete="off"></label>
        <label>Password <input type="password" id="psk${i}" maxlength="64" autocomplete="new-password"
          placeholder="${n.has_password ? 'unchanged' : (i ? 'optional backup, e.g. phone hotspot' : '')}"></label>
        <div class="row">
          <button data-save="${i}">Save &amp; connect</button>
          ${n.ssid && d.current !== i ? `<button class="ghost" data-switch="${i}">Switch to it now</button>` : ''}
        </div>
      </section>`;
    area.innerHTML = `
      <div class="grid">${d.networks.map(netForm).join('')}
        <section class="card"><h2>How Wi-Fi switching works</h2>
          <p class="muted">Only 5 GHz is used: 2.4 GHz belongs to Zigbee. If the network in use is unreachable
          for 2 minutes the hub tries the other one. Every switch from here is on trial: with no address
          within ${d.trial_seconds} s it goes back to the previous network. A phone hotspot must be set to 5 GHz.</p>
          <p class="muted">${d.custom ? 'Networks edited here are in use.' : 'Using the networks from flash.sh.'}</p>
          <div class="row">
            <button class="ghost" id="wifi-reset" ${d.custom ? '' : 'disabled'}>Reset to the networks from flash.sh</button>
          </div>
        </section>
      </div>`;
    area.querySelectorAll('[data-save]').forEach((b) => b.addEventListener('click', () => this.save(+b.dataset.save)));
    area.querySelectorAll('[data-switch]').forEach((b) => b.addEventListener('click', () => this.switchTo(+b.dataset.switch)));
    $('#wifi-reset').addEventListener('click', () => this.reset());
    this.live();
  },
  note(html, cls = '') { const n = $('#wifi-note'); if (n) n.innerHTML = html ? `<div class="note ${cls} card">${html}</div>` : ''; },
  live() {
    if (!status || !$('#wifi-note')) return;
    const t = status.wifi.trial;
    if (t === 'running') this.note(`Switching to <b>${esc(status.wifi.ssid)}</b>… on trial, reverting automatically if it doesn't get an address.`, 'warn');
    else if (this.pending && t === 'ok') { this.pending = false; this.note('Connected. The new settings are saved.', 'ok'); setTimeout(() => this.render(), 1500); }
    else if (this.pending && t === 'reverted') { this.pending = false; this.note('No address on the new network: went back to the previous one. Nothing was saved.', 'bad'); setTimeout(() => this.render(), 1500); }
  },
  async go(promise) {
    try { await promise; this.pending = true; this.note('Switching… the page may lose its connection for a moment.', 'warn'); }
    catch (e) { this.note(esc(e.message), 'bad'); }
  },
  save(target) {
    const nets = [0, 1].map((i) => ({ ssid: $(`#ssid${i}`).value.trim(), password: $(`#psk${i}`).value }));
    this.go(api('/api/wifi', { method: 'POST', json: { networks: nets, target } }));
  },
  switchTo(target) { this.go(api('/api/wifi/switch', { method: 'POST', json: { target } })); },
  reset() {
    if (confirm('Go back to the Wi-Fi networks written by flash.sh? (On trial, like any switch.)')) {
      this.go(api('/api/wifi/reset', { method: 'POST' }));
    }
  },
};

// Tailscale (Status card + Settings card) -------------------------------------------------
const tsui = {
  peers: null, peersAt: 0, shown: '',
  stateBadge(t) {
    const cls = t.state === 'connected' ? 'ok' : t.state === 'off' ? '' : t.error ? 'bad' : 'warn';
    return `<span class="badge ${cls}">${esc(t.state)}</span>`;
  },
  expiry(t) {
    if (!t.key_expiry) return '<span class="muted">never</span>';
    const d = new Date(t.key_expiry * 1000).toISOString().slice(0, 10);
    return t.key_expired ? `<span class="badge bad">expired ${d}</span>` : `${d} <span class="muted">(turn off key expiry for this machine in the admin console)</span>`;
  },
  statusCard(t) {
    if (!t) return '';
    const rows = [['Status', this.stateBadge(t) + (t.error ? `<br><span class="muted">${esc(t.error)}</span>` : '')]];
    if (t.ip) rows.push(['Tailnet IP', esc(t.ip)]);
    if (t.name) rows.push(['Name', `<a href="http://${esc(t.name)}/">${esc(t.name)}</a>`]);
    if (t.registered && t.state !== 'off') rows.push(['Node key expires', this.expiry(t)]);
    if (t.home_derp) rows.push(['Home relay (DERP)', esc(t.home_derp)]);
    if (t.ip) rows.push(['Online after boot', t.connected_ms >= 0 ? `${(t.connected_ms / 1000).toFixed(1)} s` : '–']);
    let peers = '';
    if (t.ip) {
      if (Date.now() - this.peersAt > 10000) this.loadPeers();
      const list = this.peers || [];
      peers = list.length
        ? `<table><tr><th>Peer</th><th>IP</th><th>Path</th></tr>${list.map((p) => `<tr><td>${esc(p.name.split('.')[0])}</td><td>${esc(p.ip)}</td>
            <td>${this.path(p)}</td></tr>`).join('')}</table>`
        : `<p class="muted">${this.peers ? 'No peers.' : 'Loading peers…'}</p>`;
    }
    return `<section class="card"><h2>Tailscale</h2>${dl(rows)}${peers}</section>`;
  },
  // Offline comes from the control server; direct = a working direct UDP path;
  // otherwise an online peer is reached through a DERP relay.
  path(p) {
    if (p.online === false) return '<span class="muted">offline</span>';
    if (p.direct) return '<span class="badge ok">direct</span>';
    if (p.online === true) return `<span class="badge warn">relay (DERP${p.derp ? ' ' + esc(p.derp) : ''})</span>`;
    return '<span class="muted">unknown</span>';
  },
  async loadPeers() {
    this.peersAt = Date.now();
    try { this.peers = (await api('/api/ts')).peer_list; if (currentPage() === 'status') pages.status.live(); } catch (_) { /* keep last */ }
  },
  // Rewritten only when it changes: a re-created link flashes from unvisited to visited colour.
  msg(html, cls = '') {
    const m = $('#ts-msg');
    if (!m || (m.dataset.html === html && m.className === cls)) return;
    m.dataset.html = html; m.className = cls; m.innerHTML = html;
  },
  // Settings card. Re-rendered only when what it shows changes (not on every status push),
  // so a half-typed key is never wiped.
  renderSettings() {
    const el = $('#ts-card');
    if (!el || !status) return;
    const t = status.tailscale;
    this.shown = `${t.enabled}|${t.registered}|${t.key_pending}|${t.suspended}`;
    let body;
    if (t.suspended) {
      body = `<p class="err">Suspended: the hub crashed twice in a row while Tailscale was running, so it was
        not started this time (Zigbee and automations come first). Download the log for details.</p>
        <div class="row"><button id="ts-on">Try again</button><button class="ghost" id="ts-off">Turn off</button></div>`;
    } else if (!t.enabled) {
      body = `<p class="muted">Off: the hub is reachable only on the local network.</p>
        <div class="row"><button id="ts-on">Turn on</button></div>`;
    } else if (!t.registered) {
      body = `<p class="muted">Put the hub on your tailnet so this page opens from anywhere.
        Create a key at <a href="https://login.tailscale.com/admin/settings/keys" target="_blank" rel="noopener">login.tailscale.com → Settings → Keys</a>:
        not reusable, not ephemeral, pre-approved. It is used once and then erased from the hub.</p>
        <label>Auth key <input type="password" id="ts-key" autocomplete="off" placeholder="${t.key_pending ? 'stored — paste a new one to replace it' : 'tskey-auth-…'}"></label>
        <div class="row"><button id="ts-save">Connect</button><button class="ghost" id="ts-off">Turn off</button></div>`;
    } else {
      body = `<p class="muted">On the tailnet as <b>${esc(t.name || status.hostname)}</b>. The tailnet decides who can reach it
        (Tailscale ACLs).</p>
        <div id="ts-access"><p class="muted">Loading access mode…</p></div>
        <div class="row"><button class="ghost" id="ts-off">Turn off</button><button class="danger" id="ts-logout">Log out</button></div>`;
      setTimeout(() => this.loadAccess(), 0);
    }
    el.innerHTML = `<h2>Tailscale ${this.stateBadge(t)}</h2>${body}<p id="ts-msg"></p>`;
    const on = (id, fn) => { const b = $(id); if (b) b.addEventListener('click', fn); };
    on('#ts-on', () => this.post('/api/ts/enabled', { on: true }));
    on('#ts-off', () => {
      if (t.state !== 'off' && !confirm('Turn Tailscale off? The hub restarts to apply it (Zigbee is back within seconds).')) return;
      this.post('/api/ts/enabled', { on: false });
    });
    on('#ts-logout', () => {
      if (!confirm('Log out of the tailnet? The hub forgets its node key and restarts; you will need a new auth key. Delete the old machine in the Tailscale admin console.')) return;
      this.post('/api/ts/logout');
    });
    on('#ts-save', () => {
      const key = $('#ts-key').value.trim();
      if (!key) return;
      this.post('/api/ts/key', { key }).then((ok) => { const k = $('#ts-key'); if (ok && k) k.value = ''; });
    });
    this.liveSettings();
  },
  // "Tailnet only, no password" (GET/POST /api/access)
  async loadAccess() {
    const el = $('#ts-access');
    if (!el) return;
    let a;
    try { a = await api('/api/access'); } catch (e) { el.innerHTML = `<p class="err">${esc(e.message)}</p>`; return; }
    const name = (status && status.tailscale.name) || 'the hub’s tailnet name';
    if (a.tailnet_only) {
      el.innerHTML = `<p><b>Access: tailnet only.</b> Over Tailscale no password is asked; the LAN is closed
          (port 80 refused, nothing announced).${a.lan_window_s > 0 ? ` <span class="badge warn">LAN open with password for ${Math.ceil(a.lan_window_s / 60)} more min (BOOT button)</span>` : ''}</p>
        <p class="muted">Locked out? Hold the board’s BOOT button 10 s (LAN with password for 15 min),
          or run <code>./flash.sh --prov-only --lan-access</code> over USB.</p>
        <div class="row"><button class="ghost" id="acc-off">Allow LAN access again</button></div>`;
    } else if (!a.tailscale_up) {
      el.innerHTML = `<p class="muted">Access: LAN and tailnet, with the password. <i>Tailnet only</i> can be switched on once Tailscale is connected.</p>`;
    } else if (!a.via_tailnet) {
      el.innerHTML = `<p class="muted">Access: LAN and tailnet, with the password. To switch to <i>tailnet only, no password</i>,
        open this page over Tailscale (<a href="http://${esc(name)}/">http://${esc(name)}/</a>), so the switch itself proves that path works.</p>`;
    } else {
      el.innerHTML = `<p>Access: LAN and tailnet, with the password.</p>
        <p class="muted"><b>Tailnet only</b>: the hub stops answering on the LAN and stops asking for a password over
          Tailscale — being on your tailnet is the login. Restrict <code>${esc(status.hostname)}</code> with a Tailscale ACL if your tailnet is shared.</p>
        <label>Admin password, once more <input type="password" id="acc-pw" autocomplete="current-password"></label>
        <div class="row"><button id="acc-on">Switch to tailnet only</button></div>`;
    }
    const b1 = $('#acc-on');
    if (b1) b1.addEventListener('click', () => this.setAccess({ tailnet_only: true, password: $('#acc-pw').value }));
    const b2 = $('#acc-off');
    if (b2) b2.addEventListener('click', () => {
      if (!confirm('Open the LAN again? The UI will ask for the password on the local network.')) return;
      this.setAccess({ tailnet_only: false });
    });
  },
  async setAccess(json) {
    try {
      await api('/api/access', { method: 'POST', json });
      this.loadAccess();
    } catch (e) { this.msg(esc(e.message), 'err'); }
  },
  liveSettings() {
    if (!$('#ts-card') || !status) return;
    const t = status.tailscale;
    if (`${t.enabled}|${t.registered}|${t.key_pending}|${t.suspended}` !== this.shown) { this.renderSettings(); return; }
    const h = $('#ts-card h2'), head = `Tailscale ${this.stateBadge(t)}`;
    if (h && h.dataset.html !== head) { h.dataset.html = head; h.innerHTML = head; }
    if (t.error) this.msg(esc(t.error), 'err');
    else if (t.state === 'connected') this.msg(`Connected: <code>${esc(t.ip)}</code>${t.name ? ` · <a href="http://${esc(t.name)}/">${esc(t.name)}</a>` : ''}`);
  },
  async post(path, json) {
    try {
      const r = await api(path, { method: 'POST', json });
      if (r.restarting) { this.msg('Restarting…'); fw.waitBack(null); return true; }
      await refresh();
      this.renderSettings();
      return true;
    } catch (e) { this.msg(esc(e.message), 'err'); return false; }
  },
};

// Firmware update and restart (part of Settings) ---------------------------------------
const fw = {
  busy: false,
  msg(html, cls = '') { const m = $('#fw-msg'); if (m) { m.className = cls; m.innerHTML = html; } },
  upload() {
    const file = $('#fw-file').files[0];
    if (!file || this.busy) return;
    this.busy = true;
    const before = status && status.firmware.build_id;
    $('#fw-upload').disabled = true;
    $('#fw-progress').hidden = false;
    this.msg('Uploading…');
    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/ota');
    xhr.setRequestHeader('X-Hub', '1');
    xhr.setRequestHeader('Content-Type', 'application/octet-stream');
    xhr.upload.onprogress = (e) => { if (e.lengthComputable) $('#fw-bar').value = (100 * e.loaded) / e.total; };
    xhr.onload = () => {
      let body = {};
      try { body = JSON.parse(xhr.responseText); } catch (_) { /* empty */ }
      if (xhr.status === 200) { this.msg('Installed. Restarting into the new firmware…'); this.waitBack(before); }
      else { this.busy = false; $('#fw-upload').disabled = false; this.msg(esc(body.error || `HTTP ${xhr.status}`), 'err'); }
    };
    xhr.onerror = () => { this.busy = false; $('#fw-upload').disabled = false; this.msg('Upload failed (connection lost).', 'err'); };
    xhr.send(file);
  },
  // Poll until the hub answers again, then report what it's running.
  waitBack(before) {
    const t0 = Date.now();
    const tick = async () => {
      if (Date.now() - t0 > 180000) { this.busy = false; this.msg('The hub hasn\'t come back after 3 minutes.', 'err'); return; }
      try {
        const s = await api('/api/status');
        status = s;
        this.busy = false;
        let text, cls = '';
        if (before === null) text = 'Back online.';
        else if (s.firmware.build_id !== before && s.firmware.last_update.result === 'updated') text = `Running the new firmware (${esc(s.firmware.build_id.slice(0, 8))}). ${s.firmware.on_trial ? 'Confirming…' : ''}`;
        else if (s.firmware.last_update.result === 'rolled_back') { text = 'The new firmware failed and the hub rolled back to the previous version.'; cls = 'err'; }
        else { text = 'Back online, but still on the old firmware.'; cls = 'err'; }
        if (currentPage() === 'settings') pages.settings.render();
        this.msg(text, cls);
      } catch (_) { setTimeout(tick, 2000); }
    };
    setTimeout(tick, 4000);
  },
};

// ---- boot ------------------------------------------------------------------------------
async function start() {
  const sess = await api('/api/session');
  $('#login-title').textContent = sess.hostname;
  if (!sess.authed) { showLogin(); return; }
  $('#login').hidden = true;
  $('#app').hidden = false;
  await refresh();
  render();
  if (!ws) connectWs();
}
start().catch(() => showLogin());
