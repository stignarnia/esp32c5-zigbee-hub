'use strict';
// Zigbee Hub web UI. No framework: a hash router, one status object refreshed over
// a WebSocket every 2 s, and three pages (System, Wi-Fi, Firmware).

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
const dl = (rows) => `<dl>${rows.map(([k, v]) => `<dt>${esc(k)}</dt><dd>${v}</dd>`).join('')}</dl>`;

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
    status = JSON.parse(ev.data);
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
const currentPage = () => (location.hash.slice(1) || 'system');

function render() {
  const name = currentPage();
  document.querySelectorAll('nav a').forEach((a) =>
    a.classList.toggle('active', a.getAttribute('href') === `#${name}`));
  (pages[name] || pages.system).render();
}
window.addEventListener('hashchange', render);

// System ------------------------------------------------------------------------------
pages.system = {
  render() { $('#view').innerHTML = '<div class="grid" id="sys"></div>'; this.live(); },
  live() {
    const el = $('#sys');
    if (!el || !status) return;
    const s = status, w = s.wifi, m = s.memory, f = s.firmware;
    const intTotal = 320 * 1024;
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
        ['Disconnects', `${w.disconnects}${w.last_reason ? ` <span class="muted">(last reason ${w.last_reason})</span>` : ''}`],
        ['AP not found (201)', w.no_ap_found],
        ['Network failovers', w.failovers],
        ['Driver restarts', w.driver_restarts],
        ['Watchdog reboots', w.watchdog_reboots],
      ])}</section>
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
        ['Firmware', `${esc(f.version)} <span class="muted">${esc(f.build_id.slice(0, 8))}</span>`],
        ['Built', esc(f.built)],
        ['ESP-IDF', esc(f.idf)],
      ])}</section>
      <section class="card"><h2>Diagnostic log</h2>
        <p class="muted">Warnings, errors, Wi-Fi events, updates and reboots, plus a sample every
        5 minutes, kept on the hub (the last ~500 KB) with UTC timestamps.</p>
        <div class="row"><a class="button" href="/api/logs" download>Download log</a></div>
      </section>`;
  },
};

// Wi-Fi ---------------------------------------------------------------------------------
pages.wifi = {
  data: null,
  async render() {
    $('#view').innerHTML = '<div class="card">Loading…</div>';
    try { this.data = await api('/api/wifi'); } catch (e) { $('#view').innerHTML = `<div class="card err">${esc(e.message)}</div>`; return; }
    const d = this.data;
    const netForm = (n, i) => `
      <section class="card"><h2>${i === 0 ? 'Primary network' : 'Backup network'}
        ${d.current === i && d.connected ? '<span class="badge ok">in use</span>' : ''}</h2>
        <label>SSID <input type="text" id="ssid${i}" value="${esc(n.ssid)}" maxlength="32" autocomplete="off"></label>
        <label>Password <input type="password" id="psk${i}" maxlength="64" autocomplete="new-password"
          placeholder="${n.has_password ? 'unchanged' : (i ? 'optional backup, e.g. phone hotspot' : '')}"></label>
        <div class="row">
          <button data-save="${i}">Save &amp; connect</button>
          ${n.ssid && d.current !== i ? `<button class="ghost" data-switch="${i}">Switch to it now</button>` : ''}
        </div>
      </section>`;
    $('#view').innerHTML = `
      <div id="wifi-note"></div>
      <div class="grid">${d.networks.map(netForm).join('')}</div>
      <section class="card">
        <p class="muted">Only 5 GHz is used: 2.4 GHz belongs to Zigbee. If the network in use is unreachable
        for 2 minutes the hub tries the other one. Every switch from here is on trial: with no address
        within ${d.trial_seconds} s it goes back to the previous network. A phone hotspot must be set to 5 GHz.</p>
        <div class="row">
          <button class="ghost" id="wifi-reset" ${d.custom ? '' : 'disabled'}>Reset to the networks from flash.sh</button>
          ${d.custom ? '<span class="muted">Networks edited here are in use.</span>' : '<span class="muted">Using the networks from flash.sh.</span>'}
        </div>
      </section>`;
    $('#view').querySelectorAll('[data-save]').forEach((b) => b.addEventListener('click', () => this.save(+b.dataset.save)));
    $('#view').querySelectorAll('[data-switch]').forEach((b) => b.addEventListener('click', () => this.switchTo(+b.dataset.switch)));
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

// Firmware ------------------------------------------------------------------------------
pages.firmware = {
  busy: false,
  render() {
    $('#view').innerHTML = `
      <div class="grid">
        <section class="card"><h2>Running firmware</h2><div id="fw-info"></div></section>
        <section class="card"><h2>Update</h2>
          <p class="muted">Upload <code>build/esp32c5-zigbee-hub.bin</code>, or run <code>./flash.sh --ota</code>.
          The new firmware runs on trial and the hub goes back to this version by itself if it fails.</p>
          <input type="file" id="fw-file" accept=".bin">
          <div class="row"><button id="fw-upload" disabled>Upload &amp; install</button></div>
          <div id="fw-progress" class="row" hidden><progress id="fw-bar" max="100" value="0"></progress></div>
          <p id="fw-msg"></p>
        </section>
        <section class="card"><h2>Restart</h2>
          <div class="row"><button class="danger" id="reboot">Restart the hub</button></div>
        </section>
      </div>`;
    $('#fw-file').addEventListener('change', () => { $('#fw-upload').disabled = !$('#fw-file').files.length; });
    $('#fw-upload').addEventListener('click', () => this.upload());
    $('#reboot').addEventListener('click', async () => {
      if (!confirm('Restart the hub now?')) return;
      try { await api('/api/reboot', { method: 'POST' }); this.msg('Restarting…'); this.waitBack(null); }
      catch (e) { this.msg(esc(e.message), 'err'); }
    });
    this.live();
  },
  msg(html, cls = '') { const m = $('#fw-msg'); if (m) { m.className = cls; m.innerHTML = html; } },
  live() {
    const el = $('#fw-info');
    if (!el || !status) return;
    const f = status.firmware, u = f.last_update;
    const last = u.result === 'updated' ? `<span class="badge ok">installed</span> ${esc(u.to_version)} <span class="muted">${esc(u.to.slice(0, 8))}</span>`
      : u.result === 'trial' ? `<span class="badge">confirming</span> ${esc(u.to_version)} <span class="muted">${esc(u.to.slice(0, 8))}</span>`
      : u.result === 'rolled_back' ? `<span class="badge bad">rolled back</span> <span class="muted">${esc(u.to.slice(0, 8))} failed</span>`
      : '<span class="muted">none yet</span>';
    el.innerHTML = dl([
      ['Version', esc(f.version)],
      ['Build', `<code>${esc(f.build_id)}</code>`],
      ['Built', esc(f.built)],
      ['Slot', esc(f.partition)],
      ['State', f.on_trial ? '<span class="badge warn">on trial</span>' : '<span class="badge ok">confirmed</span>'],
      ['Last update', last],
    ]);
  },
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
        if (before === null) this.msg('Back online.');
        else if (s.firmware.build_id !== before && s.firmware.last_update.result === 'updated') this.msg(`Running the new firmware (${esc(s.firmware.build_id.slice(0, 8))}). ${s.firmware.on_trial ? 'Confirming…' : ''}`);
        else if (s.firmware.last_update.result === 'rolled_back') this.msg('The new firmware failed and the hub rolled back to the previous version.', 'err');
        else this.msg('Back online, but still on the old firmware.', 'err');
        this.render();
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
