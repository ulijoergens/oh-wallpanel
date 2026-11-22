// oh-wallpanel web UI: hash-routed single page talking to /api/*.
'use strict';

const $ = (sel) => document.querySelector(sel);
const PAGES = ['home', 'settings', 'status'];
let timer = null;

async function api(path, body) {
  const opt = body === undefined ? {} : {
    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
  };
  const r = await fetch('/api/' + path, opt);
  const data = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(data.error || ('HTTP ' + r.status));
  return data;
}

function duration(s) {
  s = Math.floor(s);
  const d = Math.floor(s / 86400), h = Math.floor(s / 3600) % 24, m = Math.floor(s / 60) % 60;
  if (d) return `${d}d ${h}h`;
  if (h) return `${h}h ${m}m`;
  if (m) return `${m}m ${s % 60}s`;
  return `${s}s`;
}

function wifiText(w) {
  if (w.state === 'connected') return [`Connected`, `${w.ssid} · ${w.ip} · ${w.rssi} dBm`];
  if (w.state === 'access point') {
    const retry = w.ssid ? ` · retrying "${w.ssid}" in ${duration(w.next_retry_s)}` : '';
    return ['Access point', `${w.ap_ssid} · ${w.ap_ip}${retry}`];
  }
  if (w.state === 'reconnecting' || w.state === 'connecting') {
    return ['Reconnecting', `${w.ssid} · lost ${duration(w.lost_for_s)} · next try ${duration(w.next_retry_s)}`];
  }
  return [w.state, ''];
}

// Small DOM helper: el('td', {className: 'num'}, 'text', childNode, ...)
function el(tag, props, ...children) {
  const e = Object.assign(document.createElement(tag), props || {});
  for (const c of children) if (c !== null && c !== undefined) e.append(c);
  return e;
}

function ohText(o) {
  if (o.state === 'not configured') return ['Not configured', 'Enter the openHAB URL and token in Settings'];
  if (o.state === 'connected') return ['Connected', `${o.url}${o.version ? ' · openHAB ' + o.version : ''}`];
  return [o.state.charAt(0).toUpperCase() + o.state.slice(1), `${o.url}${o.last_error ? ' · ' + o.last_error : ''}`];
}

async function refreshHome() {
  const s = await api('status');
  header(s);
  const [w, wsub] = wifiText(s.wifi);
  $('#h-wifi').textContent = w; $('#h-wifi-sub').textContent = wsub;
  const [o, osub] = ohText(s.openhab);
  $('#h-oh').textContent = o; $('#h-oh-sub').textContent = osub;
  $('#h-uptime').textContent = duration(s.device.uptime_s);
  $('#h-reset').textContent = 'last start: ' + s.device.reset_reason;
  $('#h-log').textContent = String(s.log.count);
  $('#h-ap-banner').hidden = !(s.wifi.ap_active && s.wifi.state !== 'connected');
}

async function refreshStatus() {
  const [s, l] = await Promise.all([api('status'), api('log')]);
  header(s);
  const [w, wsub] = wifiText(s.wifi);
  const rows = [
    ['Panel', s.device.name],
    ['Time', s.device.time ? new Date(s.device.time * 1000).toLocaleString() : 'not synchronised yet'],
    ['Firmware', `${s.device.firmware} · ESP-IDF ${s.device.idf} · slot ${s.device.partition}`],
    ['Uptime', `${duration(s.device.uptime_s)} · last start: ${s.device.reset_reason}`],
    ['Free memory', `internal ${(s.device.heap_free / 1024).toFixed(0)} KB (lowest ${(s.device.heap_min / 1024).toFixed(0)} KB) · PSRAM ${(s.device.psram_free / 1048576).toFixed(1)} MB`],
    ['WiFi', `${w}${wsub ? ' · ' + wsub : ''}`],
    ['Connection attempts', s.wifi.state === 'connected' ? '–' : String(s.wifi.attempts)],
    ['openHAB', ohText(s.openhab).join(' · ')],
    ['Warnings / errors', String(s.log.count)],
  ];
  const t = $('#status-table');
  t.replaceChildren(...rows.map(([k, v]) => {
    const tr = document.createElement('tr');
    const th = document.createElement('th'); th.textContent = k;
    const td = document.createElement('td'); td.textContent = v;
    tr.append(th, td);
    return tr;
  }));

  const prev = l.previous || [];
  $('#prev-info').textContent = prev.length
    ? `The last ${prev.length} warnings/errors from earlier starts (this is start #${l.boot}).`
    : `No stored warnings or errors from earlier starts (this is start #${l.boot}).`;
  $('#prev-table').hidden = !prev.length;
  $('#prev-body').replaceChildren(...prev.map((e) => el('tr', {},
    el('td', { className: 'num' }, '#' + e.boot),
    el('td', { className: 'num' }, e.time ? new Date(e.time * 1000).toLocaleString() : `${duration(e.uptime_s)} after start`),
    el('td', { className: 'lvl-' + e.level }, e.level),
    el('td', {}, e.text))));

  const body = $('#log-body');
  if (!l.entries.length) {
    const tr = document.createElement('tr');
    const td = document.createElement('td'); td.colSpan = 3; td.className = 'muted';
    td.textContent = 'No warnings or errors since the last start.';
    tr.append(td);
    body.replaceChildren(tr);
    return;
  }
  body.replaceChildren(...l.entries.map((e) => {
    const tr = document.createElement('tr');
    const when = document.createElement('td');
    when.textContent = e.time ? new Date(e.time * 1000).toLocaleString()
                              : `${duration((l.uptime_ms - e.uptime_ms) / 1000)} ago`;
    const lvl = document.createElement('td'); lvl.textContent = e.level; lvl.className = 'lvl-' + e.level;
    const txt = document.createElement('td'); txt.textContent = e.text;
    tr.append(when, lvl, txt);
    return tr;
  }));
}

async function loadOta() {
  const o = await api('ota');
  const slot = (s) => s && s.version ? `${s.version} (${s.label}, built ${s.built}${s.state ? ', ' + s.state : ''})` : '–';
  const rows = [['Running', slot(o.running)], ['Other slot', slot(o.other)]];
  if (o.last_invalid) rows.push(['Last rejected firmware', slot(o.last_invalid)]);
  rows.push(['Maximum size', `${(o.max_size / 1024).toFixed(0)} KB`]);
  $('#ota-table').replaceChildren(...rows.map(([k, v]) => el('tr', {}, el('th', {}, k), el('td', {}, v))));
  return o;
}

function otaMsg(text, cls) { const m = $('#ota-msg'); m.textContent = text; m.className = 'msg ' + (cls || ''); }

$('#btn-ota').addEventListener('click', () => {
  const file = $('#ota-file').files[0];
  if (!file) { otaMsg('Choose the firmware file first.', 'err'); return; }
  if (!confirm(`Install ${file.name} (${(file.size / 1024).toFixed(0)} KB) and restart the panel?`)) return;
  const btn = $('#btn-ota'), bar = $('#ota-progress');
  btn.disabled = true; bar.hidden = false; bar.value = 0;
  otaMsg('Uploading…');
  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/ota');
  xhr.setRequestHeader('Content-Type', 'application/octet-stream');
  xhr.upload.onprogress = (e) => { if (e.lengthComputable) bar.value = (100 * e.loaded) / e.total; };
  xhr.onload = async () => {
    let r = {};
    try { r = JSON.parse(xhr.responseText); } catch (e) { /* ignore */ }
    if (xhr.status !== 200) { otaMsg(r.error || `Upload failed (HTTP ${xhr.status})`, 'err'); btn.disabled = false; bar.hidden = true; return; }
    otaMsg(`Firmware ${r.version} installed. Restarting…`, 'ok');
    for (let i = 0; i < 60; i++) {
      await new Promise((res) => setTimeout(res, 2000));
      try {
        const o = await loadOta();
        if (o.running.version === r.version) { otaMsg(`Running firmware ${r.version} now.`, 'ok'); break; }
        if (i > 15) { otaMsg(`The panel is back but runs ${o.running.version}: the new firmware was rolled back.`, 'err'); break; }
      } catch (e) { /* still restarting */ }
    }
    btn.disabled = false; bar.hidden = true;
  };
  xhr.onerror = () => { otaMsg('Upload failed: connection lost.', 'err'); btn.disabled = false; bar.hidden = true; };
  xhr.send(file);
});

function signalText(rssi) {
  const bars = rssi >= -55 ? '▂▄▆█' : rssi >= -67 ? '▂▄▆ ' : rssi >= -78 ? '▂▄  ' : '▂   ';
  const word = rssi >= -55 ? 'excellent' : rssi >= -67 ? 'good' : rssi >= -78 ? 'fair' : 'weak';
  return `${bars} ${rssi} dBm (${word})`;
}

async function scanWifi() {
  const m = $('#wifi-scan-msg'), list = $('#wifi-list'), btn = $('#btn-wifi-scan');
  btn.disabled = true; m.textContent = 'Searching for networks…'; m.className = 'msg';
  try {
    const { networks } = await api('wifi/scan');
    const cur = $('#settings').wifi_ssid.value;
    list.replaceChildren(...networks.map((n) => {
      const b = el('button', { type: 'button', className: n.ssid === cur ? 'selected' : '' },
        el('span', {}, (n.secured ? '🔒 ' : '') + n.ssid),
        el('span', { className: 'sig' }, signalText(n.rssi)));
      b.addEventListener('click', () => {
        const f = $('#settings');
        f.wifi_ssid.value = n.ssid;
        for (const x of list.children) x.classList.toggle('selected', x === b);
        if (n.secured) f.wifi_pass.focus();
        m.textContent = n.secured ? `Selected "${n.ssid}". Enter its password below.` : `Selected "${n.ssid}" (open network).`;
        m.className = 'msg ok';
      });
      return b;
    }));
    m.textContent = networks.length ? `${networks.length} network(s) found. Tap one to select it.` : 'No networks found.';
  } catch (e) {
    m.textContent = e.message; m.className = 'msg err';
  } finally {
    btn.disabled = false;
  }
}

$('#btn-wifi-scan').addEventListener('click', scanWifi);

async function loadSettings() {
  const s = await api('settings');
  const f = $('#settings');
  for (const el of f.elements) {
    if (!el.name || !(el.name in s)) continue;
    if (el.type === 'checkbox') el.checked = !!s[el.name];
    else el.value = s[el.name];
  }
  for (const el of f.querySelectorAll('[data-secret]')) {
    el.value = '';
    el.placeholder = s[el.dataset.secret + '_set'] ? '(stored, unchanged)' : '(none)';
  }
  f.wifi_pass_clear.checked = false;
  f.oh_token_clear.checked = false;
}

function settingsPayload(f) {
  const out = {};
  for (const el of f.elements) {
    if (!el.name) continue;
    if (el.type === 'checkbox') out[el.name] = el.checked;
    else if (el.type === 'number') out[el.name] = Number(el.value);
    else if (el.dataset.secret) { if (el.value) out[el.name] = el.value; }
    else out[el.name] = el.value.trim();
  }
  return out;
}

function msg(text, cls) {
  const m = $('#settings-msg');
  m.textContent = text;
  m.className = 'msg ' + (cls || '');
}

async function waitForRestart() {
  // Poll until the device answers again (it may come back on another address if WiFi changed).
  for (let i = 0; i < 40; i++) {
    await new Promise((r) => setTimeout(r, 1500));
    try { await api('status'); msg('Restarted with the new settings.', 'ok'); loadSettings(); return; } catch (e) { /* still down */ }
  }
  msg('The device did not come back on this address. If you changed the WiFi settings, find it on the new network.', 'err');
}

$('#settings').addEventListener('submit', async (ev) => {
  ev.preventDefault();
  const f = ev.target;
  if (!f.checkValidity()) { f.reportValidity(); msg('Please correct the highlighted fields.', 'err'); return; }
  const btn = f.querySelector('button[type=submit]');
  btn.disabled = true;
  try {
    await api('settings', settingsPayload(f));
    msg('Saved. Restarting…', 'ok');
    waitForRestart();
  } catch (e) {
    msg(e.message, 'err');
  } finally {
    btn.disabled = false;
  }
});

$('#btn-reboot').addEventListener('click', async () => {
  if (!confirm('Restart the device?')) return;
  try { await api('reboot', {}); msg('Restarting…', 'ok'); waitForRestart(); } catch (e) { msg(e.message, 'err'); }
});

$('#btn-log-clear').addEventListener('click', async () => {
  if (!confirm('Clear the stored log of earlier starts?')) return;
  await fetch('/api/log', { method: 'DELETE' });
  refreshStatus();
});

// ------------------------------------------------------------- shell

function header(s) {
  $('#hdr-name').textContent = s.device.name;
  document.title = s.device.name;
  const dot = $('#hdr-dot');
  dot.className = 'dot ' + (s.wifi.state === 'connected' ? 'ok' : s.wifi.state === 'access point' ? 'warn' : 'err');
}

function show() {
  const page = (location.hash.replace(/^#\/?/, '') || 'home').split('/')[0];
  const name = PAGES.includes(page) ? page : 'home';
  for (const p of PAGES) $('#page-' + p).hidden = p !== name;
  for (const a of document.querySelectorAll('nav a[data-page]')) a.classList.toggle('active', a.dataset.page === name);

  clearInterval(timer);
  const refresh = { home: refreshHome, status: refreshStatus }[name];
  if (refresh) {
    const run = () => refresh().catch(() => { $('#hdr-dot').className = 'dot err'; });
    run();
    timer = setInterval(run, 2000);
  } else {
    msg('');
    loadSettings().catch((e) => msg(e.message, 'err'));
    loadOta().catch((e) => otaMsg(e.message, 'err'));
    // In access-point mode (first setup, captive portal) list the networks right away.
    api('status').then((s) => { if (s.wifi.ap_active && s.wifi.state !== 'connected') scanWifi(); }).catch(() => {});
  }
}

window.addEventListener('hashchange', show);
show();
