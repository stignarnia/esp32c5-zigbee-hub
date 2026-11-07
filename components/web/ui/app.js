'use strict';
// Zigbee Hub web UI. No framework: a hash router, one status object refreshed over
// a WebSocket every 2 s, and three pages: Devices, Status (read-only) and Settings
// (everything that changes something).

const $ = (sel) => document.querySelector(sel);
const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) =>
  ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

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

// Settings: everything that changes something. Wi-Fi networks, firmware update, restart,
// diagnostic log download.
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
      </div>`;
    $('#fw-file').addEventListener('change', () => { $('#fw-upload').disabled = !$('#fw-file').files.length; });
    $('#fw-upload').addEventListener('click', () => fw.upload());
    $('#reboot').addEventListener('click', async () => {
      if (!confirm('Restart the hub now?')) return;
      try { await api('/api/reboot', { method: 'POST' }); fw.msg('Restarting…'); fw.waitBack(null); }
      catch (e) { fw.msg(esc(e.message), 'err'); }
    });
    wifi.render();
    tsui.renderSettings();
  },
  live() { wifi.live(); tsui.liveSettings(); },
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
  msg(html, cls = '') { const m = $('#ts-msg'); if (m) { m.className = cls; m.innerHTML = html; } },
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
    const h = $('#ts-card h2');
    if (h) h.innerHTML = `Tailscale ${this.stateBadge(t)}`;
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
