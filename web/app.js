'use strict';
var LANGS = ['ko', 'en', 'zh', 'ja'];
var dict = {}, fallback = {}, cur = 'en';

function t(key, p) {
  var s = dict[key] || fallback[key] || key;
  if (p) s = s.replace(/\{(\w+)\}/g, function (m, k) { return p[k] != null ? String(p[k]) : m; });
  return s;
}

function pickLang() {
  var s = null;
  try { s = localStorage.getItem('lang'); } catch (e) {}
  if (LANGS.indexOf(s) >= 0) return s;
  var n = (navigator.language || 'en').toLowerCase();
  if (n.indexOf('ko') === 0) return 'ko';
  if (n.indexOf('ja') === 0) return 'ja';
  if (n.indexOf('zh') === 0) return 'zh';
  return 'en';
}

function getJson(url) {
  return fetch(url, { cache: 'no-store' }).then(function (r) {
    if (!r.ok) throw new Error(String(r.status));
    return r.json();
  });
}

function applyI18n() {
  document.documentElement.lang = cur === 'zh' ? 'zh-Hans' : cur;
  var els = document.querySelectorAll('[data-i18n]');
  for (var i = 0; i < els.length; i++) els[i].textContent = t(els[i].getAttribute('data-i18n'));
  document.title = t('app.title');
}

function setLang(l) {
  cur = l;
  try { localStorage.setItem('lang', l); } catch (e) {}
  document.getElementById('lang').value = l;
  var p = l === 'en' ? Promise.resolve(fallback) : getJson('/assets/i18n/' + l + '.json').catch(function () { return {}; });
  return p.then(function (d) { dict = d; applyI18n(); });
}

function showMsg(key) {
  var m = document.getElementById('msg');
  m.hidden = !key;
  if (key) m.textContent = t(key);
}

function stateText(st) {
  if (st === 'ready') return t('state.ready');
  if (st === 'busy') return t('state.busy');
  if (st === 'no_medium') return t('state.no_medium');
  return st || '-';
}

function setText(id, v) { document.getElementById(id).textContent = v; }

var ST = null;          /* last /api/status */
var CFG = null;         /* last /api/settings */
var PAGE = 'files';
var upd = null;         /* update-check result: { v, url } or null */
var setupAppliedOnce = false;   /* latched: /api/status reports setup_applied only on the first read */
/* The machine's volume is 16 GiB. Under 512 MiB free there is no room for another design, so the SPA warns
 * before a save fails on the machine. Guarded by sd_ok: free_kb is 0 without a card, which stage.no_medium covers. */
var VOLUME_LOW_FREE_BYTES = 512 * 1024 * 1024;

function deviceState(s) {
  if (!s.sd_ok) return 'no_medium';
  return s.busy ? 'busy' : 'ready';
}

function refreshStatus() {
  return getJson('/api/status').then(function (s) {
    ST = s;
    if (s.setup_applied) setupAppliedOnce = true;   /* later reads return false: keep the banner for this page load */
    setText('host', s.host || '');
    setText('ip', s.ip || '');
    setText('n-files', s.files != null ? s.files : '-');
    setText('n-dirs', s.dirs != null ? s.dirs : '-');
    setText('state', stateText(deviceState(s)));
    setText('ver', s.version || '-');
    renderBanners();
    showMsg(null);
    return s;
  }).catch(function () { showMsg('status.unavailable'); return null; });
}

/* ---- banners ---- */
function banner(box, text) { box.appendChild(el('p', 'banner', text)); }
function renderBanners() {
  var box = document.getElementById('banners');
  while (box.firstChild) box.removeChild(box.firstChild);
  if (!ST) return;
  if (!ST.sd_ok) banner(box, t('banner.fat32'));
  if (ST.sd_ok && ST.total_kb > 0 && ST.free_kb * 1024 < VOLUME_LOW_FREE_BYTES) banner(box, t('banner.volume_low'));
  if (setupAppliedOnce) banner(box, t('banner.setup_applied'));
  if (ST.low_power) banner(box, t('banner.low_power'));
  if (ST.over_limit) banner(box, t('banner.over_limit', { n: ST.hidden }));
  else if (ST.hidden > 0) banner(box, t('banner.hidden', { n: ST.hidden }));
  if (!ST.time_synced) banner(box, t('banner.no_clock'));
  if (ST.selfcheck_failed) banner(box, t('banner.selfcheck_failed'));
  if (upd) {
    var p = el('p', 'banner', t('banner.update', { v: upd.v }) + ' ');
    var a = el('a', null, t('banner.update_open'));
    a.href = upd.url;
    a.target = '_blank';
    a.rel = 'noopener noreferrer';
    p.appendChild(a);
    box.appendChild(p);
  }
}

/* ---- update check: the one external request of the SPA ---- */
function verParts(v) {
  var m = /^v?(\d+)\.(\d+)\.(\d+)/.exec(String(v || ''));
  return m ? [+m[1], +m[2], +m[3]] : null;
}
function semverGt(a, b) {     /* true when version a is newer than b; unparsable -> false */
  var x = verParts(a), y = verParts(b);
  if (!x || !y) return false;
  for (var i = 0; i < 3; i++) if (x[i] !== y[i]) return x[i] > y[i];
  return false;
}
function releaseUrl(u) {      /* only a GitHub page of this project is ever linked */
  try {
    var o = new URL(u);
    if (o.protocol === 'https:' && o.hostname === 'github.com' && o.pathname.indexOf('/JerryHyun/SilbapRemoteStick/') === 0) return o.href;
  } catch (e) {}
  return null;
}
function checkUpdate() {
  if (!CFG || !CFG.update_check || !ST || !ST.version) return;
  fetch('https://api.github.com/repos/JerryHyun/SilbapRemoteStick/releases/latest', { cache: 'no-store' }).then(function (r) {
    if (!r.ok) throw new Error('x');
    return r.json();
  }).then(function (j) {
    var u = releaseUrl(j.html_url);
    if (u && semverGt(j.tag_name, ST.version)) { upd = { v: String(j.tag_name).replace(/^v/, ''), url: u }; renderBanners(); }
  }).catch(function () {});
}

/* ---- on load: clock and language ---- */
function syncClock() {
  var tz = -new Date().getTimezoneOffset();
  if (ST && CFG && (!ST.time_synced || CFG.tz_min !== tz)) {
    api('POST', '/api/time', { ms: Date.now(), tz_min: tz }).then(function () {
      CFG.tz_min = tz;
      return refreshStatus();
    }).catch(function () {});
  }
}
function saveLang(l) {
  return api('POST', '/api/settings', { ui_lang: l, lang: l }).then(function (c) { CFG = c; if (ST) ST.ui_lang = l; });
}

document.getElementById('lang').addEventListener('change', function (e) {
  var l = e.target.value;
  setLang(l).then(function () {
    saveLang(l).catch(function () {});
    refreshStatus();
    showPage(PAGE);
  });
});

function showPage(p) {
  PAGE = p;
  var tabs = document.querySelectorAll('.tab');
  for (var i = 0; i < tabs.length; i++) tabs[i].className = 'tab' + (tabs[i].getAttribute('data-page') === p ? ' on' : '');
  if (p === 'files') renderFiles();
  else if (p === 'settings') renderSettings();
  else renderHelp();
}
document.getElementById('tabs').addEventListener('click', function (e) {
  var p = e.target.getAttribute && e.target.getAttribute('data-page');
  if (p) showPage(p);
});

getJson('/assets/i18n/en.json').catch(function () { return {}; }).then(function (en) {
  fallback = en;
  return setLang(pickLang());
}).then(function () {
  showPage('files');
  loadDir('');
  return refreshStatus();
}).then(function (s) {
  if (!s) return null;
  return api('GET', '/api/settings').then(function (c) {
    CFG = c;
    if (!s.ui_lang) { saveLang(cur).catch(function () {}); }
    else {
      var stored = null;
      try { stored = localStorage.getItem('lang'); } catch (e) {}
      if (!stored && s.ui_lang !== cur && LANGS.indexOf(s.ui_lang) >= 0) setLang(s.ui_lang).then(function () { renderBanners(); showPage(PAGE); });
    }
    syncClock();
    checkUpdate();
  }, function () {});
});

/* ================= file manager ================= */
var FM = { path: '', data: null, hidden: false, busy: false };
var uploadQ = { running: false };

function el(tag, cls, text) {
  var e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text != null) e.textContent = text;
  return e;
}
function btn(label, fn, cls) {
  var b = el('button', cls || 'btn', label);
  b.type = 'button';
  b.addEventListener('click', fn);
  return b;
}
function joinPath(a, b) { return a ? a + '/' + b : b; }
function parentPath(p) { var i = p.lastIndexOf('/'); return i < 0 ? '' : p.slice(0, i); }
function fmtSize(n) {
  if (n < 1024) return n + ' B';
  if (n < 1048576) return (n / 1024).toFixed(1) + ' KB';
  if (n < 1073741824) return (n / 1048576).toFixed(1) + ' MB';
  return (n / 1073741824).toFixed(2) + ' GB';
}
function fmtDate(sec) {
  if (!sec) return '';
  try { return new Date(sec * 1000).toLocaleString(cur === 'zh' ? 'zh-CN' : cur); } catch (e) { return ''; }
}
function errKey(e) { return e && e.key ? e.key : 'err.network'; }

function api(method, url, body) {
  var o = { method: method, cache: 'no-store' };
  if (body) { o.body = JSON.stringify(body); o.headers = { 'Content-Type': 'application/json' }; }
  return fetch(url, o).then(function (r) {
    return r.json().catch(function () { return {}; }).then(function (j) {
      if (!r.ok) { var e = new Error(String(r.status)); e.key = j.error || 'err.io'; throw e; }
      return j;
    });
  }, function () { var e = new Error('net'); e.key = 'err.network'; throw e; });
}
function listUrl(path, hidden) { return '/api/list?path=' + encodeURIComponent(path) + (hidden ? '&hidden=1' : ''); }

function loadDir(path) {
  FM.path = path;
  return api('GET', listUrl(path, FM.hidden)).then(function (d) {
    FM.data = d;
    showMsg(null);
    renderFiles();
  }, function (e) {
    showMsg(errKey(e));
    if (path) loadDir('');
  });
}

function sortedEntries(list) {
  return list.slice().sort(function (a, b) {
    if (a.dir !== b.dir) return a.dir ? -1 : 1;
    return a.name.toLowerCase() < b.name.toLowerCase() ? -1 : a.name.toLowerCase() > b.name.toLowerCase() ? 1 : 0;
  });
}

function renderCrumbs(box) {
  var nav = el('nav', 'crumbs');
  nav.setAttribute('aria-label', 'path');
  var parts = FM.path ? FM.path.split('/') : [];
  nav.appendChild(btn(t('files.root'), function () { loadDir(''); }, 'crumb'));
  var acc = '';
  parts.forEach(function (p) {
    acc = joinPath(acc, p);
    var target = acc;
    nav.appendChild(el('span', 'sep', '/'));
    nav.appendChild(btn(p, function () { loadDir(target); }, 'crumb'));
  });
  box.appendChild(nav);
}

function fileInput(multi, dir, onFiles) {
  var i = document.createElement('input');
  i.type = 'file';
  i.hidden = true;
  if (multi) i.multiple = true;
  if (dir) { i.setAttribute('webkitdirectory', ''); i.multiple = true; }
  i.addEventListener('change', function () {
    var fs = [];
    for (var k = 0; k < i.files.length; k++) fs.push({ file: i.files[k], rel: dir ? (i.files[k].webkitRelativePath || i.files[k].name) : i.files[k].name });
    i.value = '';
    if (fs.length) onFiles(fs);
  });
  return i;
}

function renderToolbar(box) {
  var bar = el('div', 'toolbar');
  var up = fileInput(true, false, enqueue), upd = fileInput(true, true, enqueue);
  bar.appendChild(btn(t('files.new_folder'), newFolder));
  bar.appendChild(btn(t('files.upload'), function () { up.click(); }));
  bar.appendChild(btn(t('files.upload_folder'), function () { upd.click(); }));
  bar.appendChild(btn(t('files.refresh_now'), refreshNow, 'btn pri'));
  bar.appendChild(up); bar.appendChild(upd);
  var lab = el('label', 'chk');
  var cb = document.createElement('input');
  cb.type = 'checkbox';
  cb.checked = FM.hidden;
  cb.addEventListener('change', function () { FM.hidden = cb.checked; loadDir(FM.path); });
  lab.appendChild(cb);
  lab.appendChild(el('span', null, t('files.show_hidden')));
  bar.appendChild(lab);
  box.appendChild(bar);
}

function reasonText(r) {
  if (r === 'hidden') return t('reason.hidden');
  if (r === 'too_big') return t('reason.too_big');
  if (r === 'too_deep') return t('reason.too_deep');
  return '';
}

function renderRow(e) {
  var row = el('div', 'row' + (e.hidden_reason ? ' dim' : ''));
  var path = joinPath(FM.path, e.name);
  var name = el('div', 'c-name');
  var nm = e.dir ? btn(e.name, function () { loadDir(path); }, 'link dir') : el('span', 'fname', e.name);
  name.appendChild(nm);
  var rt = reasonText(e.hidden_reason);
  if (rt) name.appendChild(el('span', 'tag', rt));
  if (e.pending) name.appendChild(el('span', 'tag', t('files.pending')));
  row.appendChild(name);
  row.appendChild(el('div', 'c-mach mono', e.machine || ''));
  row.appendChild(el('div', 'c-size', e.dir ? '' : fmtSize(e.size)));
  row.appendChild(el('div', 'c-date', fmtDate(e.mtime)));
  var act = el('div', 'c-act');
  act.appendChild(btn(t('act.rename'), function () { doRename(e, path); }, 'btn sm'));
  act.appendChild(btn(t('act.move'), function () { doMove(e, path); }, 'btn sm'));
  act.appendChild(btn(t('act.delete'), function () { doDelete(e, path); }, 'btn sm warn'));
  row.appendChild(act);
  return row;
}

function renderFiles() {
  if (PAGE !== 'files') return;
  var view = document.getElementById('view');
  while (view.firstChild) view.removeChild(view.firstChild);
  if (!FM.data) { view.appendChild(el('p', 'muted', t('files.loading'))); return; }
  renderToolbar(view);
  renderCrumbs(view);
  var up = el('div', 'prog');
  up.id = 'upprog';
  up.hidden = !uploadQ.running;
  view.appendChild(up);
  var head = el('div', 'row head');
  head.appendChild(el('div', 'c-name', t('files.col_name')));
  head.appendChild(el('div', 'c-mach', t('files.col_machine')));
  head.appendChild(el('div', 'c-size', t('files.col_size')));
  head.appendChild(el('div', 'c-date', t('files.col_date')));
  head.appendChild(el('div', 'c-act'));
  view.appendChild(head);
  var list = el('div', 'list');
  var es = sortedEntries(FM.data.entries || []);
  if (!es.length) list.appendChild(el('p', 'muted', t('files.empty')));
  es.forEach(function (e) { list.appendChild(renderRow(e)); });
  view.appendChild(list);
  var d = FM.data;
  view.appendChild(el('p', 'muted small', t('files.free', { free: fmtSize(d.free || 0), total: fmtSize(d.total || 0) })));
  if (d.hidden > 0) view.appendChild(el('p', 'muted small', t('files.hidden_budget', { n: d.hidden })));
  view.ondragover = function (ev) { ev.preventDefault(); };
  view.ondrop = onDrop;
}

/* ---- actions ---- */
function fail(e) { showMsg(errKey(e)); }
function newFolder() {
  var n = window.prompt(t('prompt.folder_name'), '');
  if (!n) return;
  api('POST', '/api/mkdir', { path: FM.path, name: n }).then(function () { loadDir(FM.path); }, fail);
}
function doRename(e, path) {
  var n = window.prompt(t('prompt.new_name'), e.name);
  if (!n || n === e.name) return;
  api('POST', '/api/rename', { path: path, name: n }).then(function () { loadDir(FM.path); }, fail);
}
function doDelete(e, path) {
  var ask = e.dir
    ? api('GET', '/api/count?path=' + encodeURIComponent(path)).then(function (c) {
        return window.confirm(t('confirm_delete_dir', { name: e.name, files: c.files, dirs: c.dirs }));
      })
    : Promise.resolve(window.confirm(t('confirm_delete', { name: e.name })));
  ask.then(function (ok) {
    if (!ok) return null;
    return api('POST', '/api/delete', { path: path }).then(function () { loadDir(FM.path); });
  }).catch(fail);
}
function refreshNow() {
  api('POST', '/api/refresh').then(function () { showMsg('refresh.started'); refreshStatus(); }, function () { showMsg('refresh.failed'); });
}

/* ================= settings and help ================= */
function field(box, label, input, inline) {
  var f = el('label', 'field' + (inline ? ' inline' : ''));
  if (inline) { f.appendChild(input); f.appendChild(el('span', null, label)); }
  else { f.appendChild(el('span', null, label)); f.appendChild(input); }
  box.appendChild(f);
  return f;
}
function checkbox(on, onChange) {
  var c = document.createElement('input');
  c.type = 'checkbox';
  c.checked = !!on;
  c.addEventListener('change', function () { onChange(c.checked); });
  return c;
}
function select(opts, val, onChange) {
  var s = document.createElement('select');
  opts.forEach(function (o) {
    var op = el('option', null, o[1]);
    op.value = o[0];
    s.appendChild(op);
  });
  s.value = val;
  s.addEventListener('change', function () { onChange(s.value); });
  return s;
}
function textInput(type, ph) {
  var i = document.createElement('input');
  i.type = type;
  i.autocomplete = 'off';
  if (ph) i.placeholder = ph;
  return i;
}
function card(view, titleKey) {
  var c = el('div', 'card');
  c.appendChild(el('h2', null, t(titleKey)));
  view.appendChild(c);
  return c;
}
function saveSetting(patch, rebuilt) {
  api('POST', '/api/settings', patch).then(function (c) {
    CFG = c;
    showMsg(rebuilt ? 'settings.saved_rebuild' : 'settings.saved');
  }, function (e) { showMsg(errKey(e)); });
}

function renderSettings() {
  var view = document.getElementById('view');
  while (view.firstChild) view.removeChild(view.firstChild);
  if (!CFG) {
    view.appendChild(el('p', 'muted', t('files.loading')));
    api('GET', '/api/settings').then(function (c) { CFG = c; if (PAGE === 'settings') renderSettings(); }, function (e) { showMsg(errKey(e)); });
    return;
  }
  var c = card(view, 'settings.title');
  field(c, t('settings.rename'), checkbox(CFG.rename, function (v) { saveSetting({ rename: v }, true); }), true);
  c.appendChild(el('p', 'muted small', t('settings.rename_help')));
  field(c, t('settings.lang'), select([['ko', '한국어'], ['en', 'English'], ['zh', '简体中文'], ['ja', '日本語']], CFG.lang,
    function (v) { saveSetting({ lang: v }, true); }));
  c.appendChild(el('p', 'muted small', t('settings.lang_help')));
  field(c, t('settings.auto_refresh'), checkbox(CFG.auto_refresh, function (v) { saveSetting({ auto_refresh: v }); }), true);
  field(c, t('settings.update_check'), checkbox(CFG.update_check, function (v) { saveSetting({ update_check: v }); }), true);
  var bar = el('div', 'toolbar');
  bar.appendChild(btn(t('files.refresh_now'), refreshNow, 'btn pri'));
  c.appendChild(bar);

  var w = card(view, 'settings.wifi_title');
  w.appendChild(el('p', 'muted small', t('settings.wifi_current', { ssid: CFG.wifi_ssid || '-' })));
  var ss = textInput('text'), pw = textInput('password');
  ss.value = CFG.wifi_ssid || '';
  field(w, t('settings.wifi_ssid'), ss);
  field(w, t('settings.wifi_pw'), pw);
  var wb = el('div', 'toolbar');
  wb.appendChild(btn(t('settings.wifi_save'), function () {
    api('POST', '/api/wifi', { ssid: ss.value, password: pw.value }).then(function () {
      pw.value = '';
      CFG.wifi_ssid = ss.value;
      showMsg('settings.wifi_saved');
    }, function (e) { showMsg(errKey(e)); });
  }, 'btn pri'));
  w.appendChild(wb);

  var p = card(view, 'settings.pw_title');
  p.appendChild(el('p', 'muted small', t(ST && ST.pw_set ? 'settings.pw_state_on' : 'settings.pw_state_off')));
  var np = textInput('password');
  np.maxLength = 64;
  field(p, t('settings.pw_new'), np);
  function pwDone(key, on) {
    np.value = '';
    if (ST) ST.pw_set = on;
    showMsg(key);
    renderSettings();
  }
  var pb = el('div', 'toolbar');
  pb.appendChild(btn(t('settings.pw_set'), function () {
    if (!np.value) return;
    api('POST', '/api/password', { password: np.value }).then(function () { pwDone('settings.pw_saved', true); }, function (e) { showMsg(errKey(e)); });
  }, 'btn pri'));
  pb.appendChild(btn(t('settings.pw_clear'), function () {
    api('POST', '/api/password', { password: '' }).then(function () { pwDone('settings.pw_cleared', false); }, function (e) { showMsg(errKey(e)); });
  }, 'btn warn'));
  p.appendChild(pb);

  var k = card(view, 'settings.time_title');
  k.appendChild(el('p', 'muted small', t(ST && ST.time_synced ? 'settings.time_ok' : 'settings.time_not')));
  var kb = el('div', 'toolbar');
  kb.appendChild(btn(t('settings.time_set'), function () {
    api('POST', '/api/time', { ms: Date.now(), tz_min: -new Date().getTimezoneOffset() }).then(function () {
      if (CFG) CFG.tz_min = -new Date().getTimezoneOffset();
      return refreshStatus();
    }).then(function () { showMsg('settings.saved'); renderSettings(); }, function (e) { showMsg(errKey(e)); });
  }));
  k.appendChild(kb);

  otaCard(view);
}

/* ---- firmware update (Task 12) ---- */
var OTA = { file: null, busy: false };

function otaStatus(text) {
  var s = document.getElementById('otastatus');
  if (!s) return;
  s.hidden = !text;
  s.textContent = text || '';
}

/* The server sends short codes for the OTA gates (arm, length, too_large, busy, wrong_image) plus the usual
 * err.* keys; map them to the i18n keys the UI shows. */
function otaErrKey(k) {
  if (k === 'arm') return 'err.ota_arm';
  if (k === 'length') return 'err.length';
  if (k === 'too_large') return 'err.too_large';
  if (k === 'busy') return 'err.ota_busy';
  if (k === 'wrong_image') return 'err.wrong_image';
  return k || 'err.io';
}

function sendOta(file, onProgress) {
  return new Promise(function (resolve, reject) {
    var x = new XMLHttpRequest();
    x.open('POST', '/api/ota');
    x.upload.onprogress = function (ev) { if (ev.lengthComputable) onProgress(ev.loaded / ev.total); };
    x.onload = function () {
      if (x.status >= 200 && x.status < 300) { resolve(); return; }
      var k = 'err.io';
      try { k = JSON.parse(x.responseText).error || k; } catch (e) {}
      var er = new Error(String(x.status));
      er.key = otaErrKey(k);
      reject(er);
    };
    x.onerror = x.ontimeout = function () { var er = new Error('net'); er.key = 'err.network'; reject(er); };
    x.timeout = 180000;
    x.send(file);
  });
}

/* After a successful upload the stick restarts in about a second: poll until it answers again, then reload. */
function otaReconnect(tries) {
  if (tries > 20) return;
  setTimeout(function () {
    getJson('/api/status').then(function (s) {
      if (s && s.version) location.reload();
      else otaReconnect(tries + 1);
    }, function () { otaReconnect(tries + 1); });
  }, 3000);
}

/* Largest image the server accepts: /api/status's ota_max (min of the two slots), so no size is hardcoded here. The
 * fallback is the 4 MiB slot of this board, used only before the first /api/status or with an older firmware. */
var OTA_FALLBACK_MAX = 4 * 1024 * 1024;
function otaMaxBytes() {
  return ST && ST.ota_max > 0 ? ST.ota_max : OTA_FALLBACK_MAX;
}

/* The server gate decides from the password state LATCHED AT BOOT, not the live one: setting a password during this
 * boot does not remove the BOOT requirement. Mirror that here; an older firmware without pw_at_boot falls back to the
 * live pw_set, which is what its gate used. */
function otaBootPressNeeded() {
  if (!ST) return true;
  return ST.pw_at_boot === undefined ? !ST.pw_set : !ST.pw_at_boot;
}

function otaCard(view) {
  var c = card(view, 'settings.ota_title');
  c.appendChild(el('p', 'muted small', t('settings.ota_version', { v: (ST && ST.version) || '-' })));
  c.appendChild(el('p', 'muted small', t('settings.ota_help')));

  var pick = document.createElement('input');
  pick.type = 'file';
  pick.accept = '.bin';
  pick.hidden = true;
  var name = el('span', 'mono small', OTA.file ? OTA.file.name + ' (' + fmtSize(OTA.file.size) + ')' : t('settings.ota_none'));
  pick.addEventListener('change', function () {
    OTA.file = pick.files && pick.files[0] ? pick.files[0] : null;
    name.textContent = OTA.file ? OTA.file.name + ' (' + fmtSize(OTA.file.size) + ')' : t('settings.ota_none');
    otaStatus(null);
  });

  var bar = el('div', 'toolbar');
  bar.appendChild(btn(t('settings.ota_choose'), function () { pick.click(); }));
  if (otaBootPressNeeded()) {          /* a device that booted with a password: the upload itself is the permission */
    bar.appendChild(btn(t('settings.ota_arm'), function () {
      api('POST', '/api/ota/arm').then(function () {
        otaStatus(t('settings.ota_armed'));
      }, function (e) { showMsg(errKey(e)); });
    }));
    c.appendChild(el('p', 'muted small', t('settings.ota_arm_hint')));
  }
  var up = btn(t('settings.ota_upload'), function () {
    if (OTA.busy) return;
    if (!OTA.file) { showMsg('settings.ota_none'); return; }
    if (OTA.file.size > otaMaxBytes()) { showMsg('err.too_large'); return; }
    OTA.busy = true;
    up.disabled = true;
    otaStatus(t('settings.ota_started'));
    sendOta(OTA.file, function (frac) {
      otaStatus(t('settings.ota_uploading', { pct: Math.round(frac * 100) }));
    }).then(function () {
      OTA.busy = false;
      otaStatus(t('settings.ota_done'));
      otaReconnect(0);
    }, function (e) {
      OTA.busy = false;
      up.disabled = false;
      otaStatus(null);
      showMsg(errKey(e));
    });
  }, 'btn warn');
  bar.appendChild(up);
  bar.appendChild(pick);
  c.appendChild(bar);
  c.appendChild(name);
  var line = el('p', 'muted small');
  line.id = 'otastatus';
  line.hidden = true;
  c.appendChild(line);
}

function renderHelp() {
  var view = document.getElementById('view');
  while (view.firstChild) view.removeChild(view.firstChild);
  view.appendChild(el('h2', null, t('help.title')));
  var pre = el('pre', 'help', t('files.loading'));
  view.appendChild(pre);
  fetch('/api/help', { cache: 'no-store' }).then(function (r) {
    if (!r.ok) throw new Error(String(r.status));
    return r.text();
  }).then(function (txt) { pre.textContent = txt; }, function () { pre.textContent = t('help.failed'); });
  view.appendChild(el('p', 'muted small', t('footer.version') + ' ' + ((ST && ST.version) || '-')));
}

/* ---- move: folder picker ---- */
function doMove(e, path) {
  var ov = el('div', 'overlay');
  var box = el('div', 'dlg');
  ov.appendChild(box);
  var cur = FM.path, gen = 0;
  function close() { gen++; if (ov.parentNode) document.body.removeChild(ov); }
  function show() {
    var my = ++gen;
    while (box.firstChild) box.removeChild(box.firstChild);
    box.appendChild(el('h2', null, t('move.title', { name: e.name })));
    box.appendChild(el('p', 'mono', '/' + cur));
    var ul = el('div', 'pick');
    box.appendChild(ul);
    var acts = el('div', 'toolbar');
    acts.appendChild(btn(t('move.here'), function () {
      api('POST', '/api/move', { path: path, to: cur }).then(function () { close(); loadDir(FM.path); }, function (er) { close(); fail(er); });
    }, 'btn pri'));
    acts.appendChild(btn(t('move.cancel'), close));
    box.appendChild(acts);
    api('GET', listUrl(cur, false)).then(function (d) {
      if (my !== gen) return;
      if (cur) ul.appendChild(btn('..', function () { cur = parentPath(cur); show(); }, 'link dir'));
      sortedEntries(d.entries || []).forEach(function (x) {
        if (!x.dir || joinPath(cur, x.name) === path) return;
        ul.appendChild(btn(x.name, function () { cur = joinPath(cur, x.name); show(); }, 'link dir'));
      });
    }, function (er) { if (my !== gen) return; close(); fail(er); });
  }
  document.body.appendChild(ov);
  show();
}

/* ---- uploads: one at a time ---- */
var upState = { files: [], done: 0, total: 0, bytesDone: 0, bytesAll: 0, cur: null, dirsMade: {}, failed: 0 };

function enqueue(items) {
  items.forEach(function (it) {
    upState.files.push({ file: it.file, rel: it.rel, base: FM.path });
    upState.total++;
    upState.bytesAll += it.file.size;
  });
  if (!uploadQ.running) { uploadQ.running = true; upNext(); }
}

function ensureDirs(base, rel) {
  var parts = rel.split('/');
  parts.pop();
  var chain = Promise.resolve(base);
  parts.forEach(function (seg) {
    chain = chain.then(function (cur) {
      var want = joinPath(cur, seg);
      if (upState.dirsMade[want] != null) return upState.dirsMade[want];
      return api('POST', '/api/mkdir', { path: cur, name: seg }).then(function (r) {
        upState.dirsMade[want] = joinPath(cur, r.name);   /* server's sanitized name, new or existing */
        return upState.dirsMade[want];
      });
    });
  });
  return chain;
}

function showUp(name, frac) {
  var box = document.getElementById('upprog');
  if (!box) return;
  box.hidden = false;
  while (box.firstChild) box.removeChild(box.firstChild);
  box.appendChild(el('div', null, t('upload.file', { name: name, i: Math.min(upState.done + 1, upState.total), n: upState.total })));
  var p1 = document.createElement('progress');
  p1.max = 1; p1.value = frac;
  box.appendChild(p1);
  var all = upState.bytesAll ? (upState.bytesDone + (upState.cur ? upState.cur.size * frac : 0)) / upState.bytesAll : 1;
  box.appendChild(el('div', 'small muted', t('upload.total')));
  var p2 = document.createElement('progress');
  p2.max = 1; p2.value = all;
  box.appendChild(p2);
}

function upNext() {
  var it = upState.files.shift();
  if (!it) { upFinish(); return; }
  upState.cur = it.file;
  showUp(it.rel, 0);
  ensureDirs(it.base, it.rel).then(function (dir) {
    return sendFile(dir, it.rel.split('/').pop(), it.file);
  }).then(function () {}, function (e) {
    upState.failed++;
    showMsg(errKey(e));
    if (errKey(e) === 'err.no_space') { upState.files = []; }
  }).then(function () {
    upState.done++;
    upState.bytesDone += it.file.size;
    upState.cur = null;
    upNext();
  });
}

function upFinish() {
  var n = upState.done - upState.failed, ok = !upState.failed;
  uploadQ.running = false;
  upState = { files: [], done: 0, total: 0, bytesDone: 0, bytesAll: 0, cur: null, dirsMade: {}, failed: 0 };
  loadDir(FM.path).then(function () {
    refreshStatus();
    if (ok && n > 0) { var m = document.getElementById('msg'); m.hidden = false; m.textContent = t('upload.done', { n: n }); }
  });
}

function sendFile(dir, name, file) {
  return new Promise(function (resolve, reject) {
    var x = new XMLHttpRequest();
    x.open('POST', '/api/upload?path=' + encodeURIComponent(dir) + '&name=' + encodeURIComponent(name));
    x.setRequestHeader('X-Client-Time', String(Date.now()));
    x.setRequestHeader('X-Client-Tz', String(-new Date().getTimezoneOffset()));
    x.upload.onprogress = function (ev) { if (ev.lengthComputable) showUp(name, ev.loaded / ev.total); };
    x.onload = function () {
      if (x.status >= 200 && x.status < 300) { resolve(); return; }
      var k = 'err.io';
      try { k = JSON.parse(x.responseText).error || k; } catch (e) {}
      var er = new Error(String(x.status)); er.key = k; reject(er);
    };
    x.onerror = x.ontimeout = function () { var er = new Error('net'); er.key = 'err.network'; reject(er); };
    x.send(file);
  });
}

/* ---- drag and drop (files and folders) ---- */
function readEntries(rd) {
  return new Promise(function (res, rej) { rd.readEntries(res, rej); });
}
function walk(entry, prefix, out) {
  if (entry.isFile) {
    return new Promise(function (res) {
      entry.file(function (f) { out.push({ file: f, rel: prefix + f.name }); res(); }, function () { res(); });
    });
  }
  var rd = entry.createReader();
  function more() {
    return readEntries(rd).then(function (batch) {
      if (!batch.length) return null;
      var p = Promise.resolve();
      batch.forEach(function (b) { p = p.then(function () { return walk(b, prefix + entry.name + '/', out); }); });
      return p.then(more);
    });
  }
  return more();
}
function onDrop(ev) {
  ev.preventDefault();
  var items = ev.dataTransfer && ev.dataTransfer.items;
  if (!items) return;
  var out = [], p = Promise.resolve();
  for (var i = 0; i < items.length; i++) {
    var en = items[i].webkitGetAsEntry && items[i].webkitGetAsEntry();
    if (en) (function (e2) { p = p.then(function () { return walk(e2, '', out); }); })(en);
  }
  p.then(function () { if (out.length) enqueue(out); });
}
