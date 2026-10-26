/*
 * The shared Ardour frontend: sessions, transport, an editor timeline with
 * regions, and a mixer, over ArdourBridge (bridge.js).  Plain JavaScript, no
 * build step, so that the same files run in the Android WebView, the iOS
 * WKWebView and a browser.
 */
(function () {
  'use strict';

  var $ = function (id) { return document.getElementById(id); };
  var call = window.ArdourBridge.call;

  var LANE_HEIGHT = 64;
  var RULER_HEIGHT = 22;

  var state = {
    running: false,
    session: null,
    rolling: false,
    position: 0,
    sampleRate: 48000,
    view: 'session',
    timeline: null,
    strips: [],
    selectedTrack: null,
    selectedRegion: null,
    pxPerSec: 40,
    peaks: {},
    notes: {},        /* region id -> notes, for drawing MIDI regions */
    overlay: null,    /* what the overlay shows: {kind: 'roll'|'plugin'|'plugins', ...} */
    automation: {},   /* track id -> {control, plugin, param, data}: the automation lanes shown */
    keyboard: false,  /* the on-screen MIDI keyboard */
    octave: 4
  };

  /* ------------------------------------------------------------------ util */

  function message(text) { $('message').textContent = text || ''; }

  function checked(reply) {
    if (!reply || !reply.ok) {
      message(reply && reply.error ? reply.error : 'request failed');
      throw new Error(reply && reply.error);
    }
    message('');
    return reply;
  }

  function ignore() {}

  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = text;
    return e;
  }

  function dbToFraction(db) {
    /* -60 dBFS .. 0 dBFS onto 0 .. 1 */
    return Math.max(0, Math.min(1, (db + 60) / 60));
  }

  function pad(n, w) { n = String(n); while (n.length < w) n = '0' + n; return n; }

  function formatClock(seconds) {
    var ms = Math.floor((seconds % 1) * 1000);
    var s = Math.floor(seconds) % 60;
    var m = Math.floor(seconds / 60) % 60;
    var h = Math.floor(seconds / 3600);
    return pad(h, 2) + ':' + pad(m, 2) + ':' + pad(s, 2) + '.' + pad(ms, 3);
  }

  function formatGain(db) {
    return db <= -60 ? '-inf' : (db > 0 ? '+' : '') + db.toFixed(1);
  }

  function formatPan(pan) {
    if (Math.abs(pan) < 0.005) return 'C';
    return (pan < 0 ? 'L' : 'R') + Math.round(Math.abs(pan) * 100);
  }

  function samplesToPx(samples) { return samples / state.sampleRate * state.pxPerSec; }
  function pxToSamples(px) { return Math.max(0, Math.round(px / state.pxPerSec * state.sampleRate)); }

  /* ---------------------------------------------------------------- status */

  function applyStatus(st) {
    state.running = !!st.running;
    state.session = st.session || null;
    if (st.sample_rate) state.sampleRate = st.sample_rate;
    var badge = $('engine');
    if (st.starting) {
      badge.textContent = 'engine: starting…';
      badge.className = 'badge';
    } else if (st.running) {
      badge.textContent = st.backend + ' · ' + st.sample_rate + ' Hz · ' + st.buffer_size;
      badge.className = 'badge ok';
    } else {
      badge.textContent = 'engine: stopped';
      badge.className = 'badge err';
    }
    $('current').textContent = state.session ? state.session.name : 'no session';
    var haveSession = !!state.session;
    ['save', 'close', 'play', 'stop', 'home', 'record'].forEach(function (id) { $(id).disabled = !haveSession; });
    document.querySelectorAll('#tabs button[data-view="editor"], #tabs button[data-view="mixer"]').forEach(function (b) {
      b.disabled = !haveSession;
    });
    if (!haveSession) {
      state.timeline = null;
      state.strips = [];
      state.selectedTrack = state.selectedRegion = null;
      state.peaks = {};
      showView('session');
    }
    return st;
  }

  function refreshStatus() {
    return call('engine.status').then(function (st) {
      if (!st.ok) {
        $('engine').textContent = 'engine: ' + st.error;
        $('engine').className = 'badge err';
        return st;
      }
      return applyStatus(st);
    });
  }

  function afterSessionChange(st) {
    applyStatus(st);
    if (state.session) {
      showView('editor');
      return Promise.all([refreshSessions(), refreshAll()]);
    }
    return refreshSessions();
  }

  /* ----------------------------------------------------------------- views */

  function showView(name) {
    state.view = name;
    document.querySelectorAll('#tabs button').forEach(function (b) {
      b.classList.toggle('active', b.dataset.view === name);
    });
    document.querySelectorAll('.view').forEach(function (v) {
      v.classList.toggle('active', v.id === 'view-' + name);
    });
    if (name === 'editor') refreshTimeline();
    if (name === 'mixer') refreshStrips();
  }

  document.querySelectorAll('#tabs button').forEach(function (b) {
    b.onclick = function () { if (!b.disabled) showView(b.dataset.view); };
  });

  function refreshAll() {
    return Promise.all([refreshTimeline(), refreshStrips()]);
  }

  /* -------------------------------------------------------------- sessions */

  function refreshSessions() {
    return call('session.list').then(function (r) {
      var ul = $('session-list');
      ul.innerHTML = '';
      if (!r.ok) return;
      r.sessions.forEach(function (name) {
        var li = el('li');
        li.appendChild(el('span', '', name));
        var open = el('button', '', 'Open');
        open.onclick = function () {
          call('session.open', { name: name }).then(checked).then(afterSessionChange).catch(ignore);
        };
        li.appendChild(open);
        ul.appendChild(li);
      });
    });
  }

  $('create').onsubmit = function (e) {
    e.preventDefault();
    var name = $('new-name').value.trim();
    if (!name) return;
    call('session.create', { name: name }).then(checked).then(function (st) {
      $('new-name').value = '';
      return afterSessionChange(st);
    }).catch(ignore);
  };

  $('save').onclick = function () { call('session.save').then(checked).then(function () { message('saved'); }).catch(ignore); };
  $('close').onclick = function () { call('session.close').then(checked).then(afterSessionChange).catch(ignore); };

  /* ------------------------------------------------------------- transport */

  $('play').onclick = function () { call('transport.play').then(checked).catch(ignore); };
  $('stop').onclick = function () {
    call('transport.stop').then(checked).then(function () { setTimeout(refreshTimeline, 300); }).catch(ignore);
  };
  $('home').onclick = function () { locate(0); };
  $('record').onclick = function () {
    call('transport.record', { enabled: !state.recordEnabled }).then(checked).then(updateTransport).catch(ignore);
  };

  function locate(samples) {
    return call('transport.locate', { position: samples }).then(checked).then(function (t) {
      updateTransport(t);
    }).catch(ignore);
  }

  function updateTransport(t) {
    var wasRolling = state.rolling;
    if (wasRolling && !t.rolling) setTimeout(refreshTimeline, 500); /* a take may have landed */
    state.recordEnabled = !!t.record_enabled;
    $('record').classList.toggle('armed', !!t.record_enabled && !t.recording);
    $('record').classList.toggle('recording', !!t.recording);
    state.rolling = t.rolling;
    state.position = t.position;
    $('clock').textContent = formatClock(t.seconds);
    $('play').classList.toggle('rolling', !!t.rolling);
    $('playhead').style.left = samplesToPx(t.position) + 'px';
    if (t.rolling && state.view === 'editor') {
      /* keep the playhead in view while rolling */
      var scroll = $('lanes-scroll');
      var x = samplesToPx(t.position);
      if (x < scroll.scrollLeft || x > scroll.scrollLeft + scroll.clientWidth - 20) {
        scroll.scrollLeft = Math.max(0, x - 40);
      }
    }
  }

  /* -------------------------------------------------------------- timeline */

  function refreshTimeline() {
    if (!state.session) return Promise.resolve();
    return call('timeline').then(function (r) {
      if (!r.ok) return;
      state.timeline = r;
      state.sampleRate = r.sample_rate || state.sampleRate;
      updateHistory(r);
      var ids = r.tracks.map(function (t) { return t.id; });
      if (ids.indexOf(state.selectedTrack) < 0) state.selectedTrack = ids.length ? ids[0] : null;
      if (state.selectedRegion && !findRegion(state.selectedRegion)) state.selectedRegion = null;
      renderTimeline();
    });
  }

  function updateHistory(r) {
    $('undo').disabled = !r.undo;
    $('undo').title = r.undo ? 'Undo ' + r.undo : 'Nothing to undo';
    $('redo').disabled = !r.redo;
    $('redo').title = r.redo ? 'Redo ' + r.redo : 'Nothing to redo';
  }

  function afterHistory() {
    state.notes = {};
    if (state.overlay && state.overlay.kind === 'roll') loadRoll();
    return refreshTimeline();
  }
  $('undo').onclick = function () { call('edit.undo').then(checked).then(afterHistory).catch(ignore); };
  $('redo').onclick = function () { call('edit.redo').then(checked).then(afterHistory).catch(ignore); };

  function findRegion(id) {
    if (!state.timeline) return null;
    for (var i = 0; i < state.timeline.tracks.length; i++) {
      var t = state.timeline.tracks[i];
      for (var j = 0; j < t.regions.length; j++) {
        if (t.regions[j].id === id) return { track: t, region: t.regions[j] };
      }
    }
    return null;
  }

  function renderRuler(width) {
    var ruler = $('ruler');
    ruler.innerHTML = '';
    /* a label every 1, 2, 5, 10, 30 or 60 seconds, whichever leaves room */
    var steps = [1, 2, 5, 10, 30, 60, 120, 300];
    var step = steps[steps.length - 1];
    for (var i = 0; i < steps.length; i++) {
      if (steps[i] * state.pxPerSec >= 60) { step = steps[i]; break; }
    }
    var seconds = width / state.pxPerSec;
    for (var s = 0; s <= seconds; s += step) {
      var tick = el('span', 'tick', s >= 60 ? Math.floor(s / 60) + ':' + pad(s % 60, 2) : s + 's');
      tick.style.left = s * state.pxPerSec + 'px';
      ruler.appendChild(tick);
    }
  }

  function renderTimeline() {
    var tl = state.timeline;
    var heads = $('track-heads');
    var rows = $('lane-rows');
    heads.innerHTML = '';
    rows.innerHTML = '';
    if (!tl) return;

    var endSeconds = Math.max(60, tl.end / state.sampleRate + 20);
    var width = Math.ceil(endSeconds * state.pxPerSec);
    $('lanes').style.width = width + 'px';
    var autoLanes = tl.tracks.filter(function (t) { return state.automation[t.id]; }).length;
    $('lanes').style.height = (RULER_HEIGHT + tl.tracks.length * LANE_HEIGHT + autoLanes * AUTO_HEIGHT) + 'px';
    renderRuler(width);

    heads.appendChild(el('div', 'head-spacer'));
    tl.tracks.forEach(function (t) {
      var head = el('div', 'track-head' + (t.id === state.selectedTrack ? ' selected' : ''));
      var row = el('div', 'head-row');
      row.appendChild(el('span', 'track-title', t.name));
      var arm = el('button', 'rec-arm' + (t.rec ? ' on' : ''), 'R');
      arm.title = 'Arm for recording';
      arm.onclick = function (e) {
        e.stopPropagation();
        call('strip.set', { id: t.id, rec: !t.rec }).then(checked).then(refreshAll).catch(ignore);
      };
      row.appendChild(arm);
      var autoBtn = el('button', 'auto-toggle' + (state.automation[t.id] ? ' on' : ''), 'A');
      autoBtn.title = 'Show automation';
      autoBtn.onclick = function (e) {
        e.stopPropagation();
        if (state.automation[t.id]) delete state.automation[t.id];
        else state.automation[t.id] = { control: 'gain' };
        renderTimeline();
      };
      row.appendChild(autoBtn);
      head.appendChild(row);
      var meter = el('div', 'meter h');
      meter.dataset.meter = t.id;
      meter.appendChild(el('div'));
      head.appendChild(meter);
      head.onclick = function () { selectTrack(t.id); };
      heads.appendChild(head);

      var lane = el('div', 'lane' + (t.id === state.selectedTrack ? ' selected' : ''));
      lane.dataset.track = t.id;
      lane.addEventListener('click', function (e) {
        if (e.target !== lane) return;
        selectTrack(t.id);
        locate(pxToSamples(e.offsetX));
      });
      t.regions.forEach(function (r) { lane.appendChild(regionElement(t, r)); });
      rows.appendChild(lane);
      if (state.automation[t.id]) {
        heads.appendChild(automationHead(t));
        rows.appendChild(automationLane(t, width));
      }
    });
    $('playhead').style.left = samplesToPx(state.position) + 'px';
    updateRegionTools();
  }

  /* ------------------------------------------------------------ automation */

  var AUTO_HEIGHT = 56;
  var AUTO_STATES = ['manual', 'play', 'write', 'touch', 'latch'];

  function autoParams(t) {
    var a = state.automation[t.id];
    var p = { route: t.id, control: a.control };
    if (a.control === 'plugin') { p.plugin = a.plugin; p.param = a.param; }
    return p;
  }

  function automationHead(t) {
    var a = state.automation[t.id];
    var head = el('div', 'auto-head');
    var which = el('select', 'auto-control');
    var options = [['gain', 'Gain'], ['trim', 'Trim'], ['pan', 'Pan'], ['mute', 'Mute']];
    var strip = state.strips.filter(function (x) { return x.id === t.id; })[0];
    var key = a.control === 'plugin' ? 'plugin:' + a.plugin + ':' + a.param : a.control;
    options.forEach(function (o) {
      var opt = el('option', '', o[1]); opt.value = o[0]; which.appendChild(opt);
    });
    (strip && strip.plugins || []).forEach(function (pl) {
      (a.paramsOf && a.paramsOf[pl.id] || []).forEach(function (pm) {
        var opt = el('option', '', pl.name + ': ' + pm.name);
        opt.value = 'plugin:' + pl.id + ':' + pm.index;
        which.appendChild(opt);
      });
    });
    which.value = key;
    which.onchange = function () {
      var v = which.value.split(':');
      a.control = v[0]; a.plugin = v[1]; a.param = v[2]; a.data = null;
      renderTimeline();
    };
    /* the plugin parameters are listed once they are known */
    if (strip && strip.plugins && strip.plugins.length && !a.paramsOf) {
      a.paramsOf = {};
      Promise.all(strip.plugins.map(function (pl) {
        return call('plugin.get', { route: t.id, plugin: pl.id }).then(function (r) {
          if (r.ok) a.paramsOf[pl.id] = r.plugin.params.filter(function (pm) { return !pm.toggled; });
        });
      })).then(function () { if (state.automation[t.id] === a) renderTimeline(); });
    }
    head.appendChild(which);
    var mode = el('select', 'auto-state');
    AUTO_STATES.forEach(function (m) { var o = el('option', '', m); o.value = m; mode.appendChild(o); });
    mode.value = a.data ? a.data.state : 'manual';
    mode.onchange = function () {
      var p = autoParams(t); p.state = mode.value;
      call('automation.state', p).then(checked).then(function (r) { a.data = r; }).catch(ignore);
    };
    head.appendChild(mode);
    return head;
  }

  function automationLane(t, width) {
    var a = state.automation[t.id];
    var lane = el('div', 'auto-lane');
    lane.dataset.track = t.id;
    var svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    svg.setAttribute('width', width);
    svg.setAttribute('height', AUTO_HEIGHT);
    lane.appendChild(svg);
    var yPx = function (y) { return 4 + (1 - y) * (AUTO_HEIGHT - 8); };
    var draw = function (d) {
      while (svg.firstChild) svg.removeChild(svg.firstChild);
      var ns = 'http://www.w3.org/2000/svg';
      var pts = d.points;
      var line = document.createElementNS(ns, 'polyline');
      var coords = [];
      if (!pts.length) {
        coords.push('0,' + yPx(d.y), width + ',' + yPx(d.y));
      } else {
        coords.push('0,' + yPx(pts[0].y));
        pts.forEach(function (pt) { coords.push(samplesToPx(pt.t) + ',' + yPx(pt.y)); });
        coords.push(width + ',' + yPx(pts[pts.length - 1].y));
      }
      line.setAttribute('points', coords.join(' '));
      line.setAttribute('class', 'auto-line' + (pts.length ? '' : ' flat'));
      svg.appendChild(line);
      pts.forEach(function (pt, i) {
        var c = document.createElementNS(ns, 'circle');
        c.setAttribute('cx', samplesToPx(pt.t));
        c.setAttribute('cy', yPx(pt.y));
        c.setAttribute('r', 6);
        c.setAttribute('class', 'auto-point');
        c.addEventListener('click', function (e) {
          e.stopPropagation();
          var p = autoParams(t); p.index = i;
          call('automation.remove', p).then(checked).then(function (r) { a.data = r; draw(r); }).catch(ignore);
        });
        svg.appendChild(c);
      });
      var mode = document.querySelectorAll('.auto-head')[Array.prototype.indexOf.call(document.querySelectorAll('.auto-lane'), lane)];
      if (mode) mode.querySelector('.auto-state').value = d.state;
    };
    lane.addEventListener('click', function (e) {
      if (e.target.classList && e.target.classList.contains('auto-point')) return;
      var rect = lane.getBoundingClientRect();
      var p = autoParams(t);
      p.t = pxToSamples(e.clientX - rect.left);
      p.y = Math.max(0, Math.min(1, 1 - (e.clientY - rect.top - 4) / (AUTO_HEIGHT - 8)));
      call('automation.add', p).then(checked).then(function (r) { a.data = r; draw(r); }).catch(ignore);
    });
    if (a.data) draw(a.data);
    call('automation.get', autoParams(t)).then(function (r) {
      if (!r.ok) { message(r.error); return; }
      a.data = r;
      draw(r);
    });
    return lane;
  }

  /* ---------------------------------------------------- on-screen keyboard */

  function renderKeyboard() {
    var box = $('keyboard');
    box.hidden = !state.keyboard;
    $('keys-toggle').classList.toggle('on', state.keyboard);
    if (!state.keyboard) return;
    box.innerHTML = '';
    var bar = el('div', 'kb-bar');
    var down = el('button', '', '◀'), up = el('button', '', '▶');
    var label = el('span', 'kb-octave', 'C' + state.octave);
    down.onclick = function () { state.octave = Math.max(0, state.octave - 1); renderKeyboard(); };
    up.onclick = function () { state.octave = Math.min(8, state.octave + 1); renderKeyboard(); };
    bar.appendChild(down); bar.appendChild(label); bar.appendChild(up);
    box.appendChild(bar);
    var keys = el('div', 'kb-keys');
    var base = (state.octave + 1) * 12;
    var whites = 0;
    for (var n = base; n < base + 25; n++) if (!isBlack(n)) whites++;
    var w = 100 / whites, wi = 0;
    for (n = base; n < base + 25; n++) {
      var black = isBlack(n);
      var k = el('div', 'kb-key' + (black ? ' black' : ''));
      if (black) {
        k.style.left = (wi * w - w * 0.3) + '%';
        k.style.width = (w * 0.6) + '%';
      } else {
        k.style.left = (wi * w) + '%';
        k.style.width = w + '%';
        wi++;
      }
      attachKey(k, n);
      keys.appendChild(k);
    }
    box.appendChild(keys);
  }

  function sendNote(note, on) {
    var p = { note: note, on: on, velocity: 100 };
    if (state.selectedTrack) p.track = state.selectedTrack;
    return call('midi.send', p);
  }

  function attachKey(k, note) {
    var held = false;
    k.addEventListener('pointerdown', function (e) {
      e.preventDefault();
      k.setPointerCapture(e.pointerId);
      held = true; k.classList.add('down');
      sendNote(note, true);
    });
    var release = function () {
      if (!held) return;
      held = false; k.classList.remove('down');
      sendNote(note, false);
    };
    k.addEventListener('pointerup', release);
    k.addEventListener('pointercancel', release);
  }

  $('keys-toggle').onclick = function () { state.keyboard = !state.keyboard; renderKeyboard(); };

  function regionElement(track, r) {
    var div = el('div', 'region' + (r.kind === 'midi' ? ' midi' : '') + (r.muted ? ' muted' : '') + (r.id === state.selectedRegion ? ' selected' : ''));
    div.dataset.region = r.id;
    var left = samplesToPx(r.position);
    var width = Math.max(4, samplesToPx(r.length));
    div.style.left = left + 'px';
    div.style.width = width + 'px';
    var canvas = el('canvas');
    canvas.width = Math.min(4000, Math.max(1, Math.round(width)));
    canvas.height = LANE_HEIGHT - 22;
    div.appendChild(canvas);
    div.appendChild(el('span', 'region-name', r.name));
    if (r.kind === 'midi') drawNotes(canvas, r); else drawPeaks(canvas, r);
    if (r.fade_in !== undefined) {
      var svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
      svg.setAttribute('class', 'fades');
      div.appendChild(svg);
      drawFades(svg, width, r.fade_in, r.fade_out);
    }
    attachDrag(div, track, r);
    attachHandles(div, r, width);
    return div;
  }

  function drawFades(svg, width, fadeIn, fadeOut) {
    var h = LANE_HEIGHT - 8;
    var fi = Math.min(width, samplesToPx(fadeIn)), fo = Math.min(width, samplesToPx(fadeOut));
    svg.setAttribute('viewBox', '0 0 ' + width + ' ' + h);
    svg.setAttribute('preserveAspectRatio', 'none');
    svg.innerHTML =
      '<polygon points="0,0 ' + fi + ',0 0,' + h + '" fill="rgba(0,0,0,.45)"/>' +
      '<line x1="0" y1="' + h + '" x2="' + fi + '" y2="0" stroke="#e0c050" stroke-width="1.5"/>' +
      '<polygon points="' + width + ',0 ' + (width - fo) + ',0 ' + width + ',' + h + '" fill="rgba(0,0,0,.45)"/>' +
      '<line x1="' + (width - fo) + '" y1="0" x2="' + width + '" y2="' + h + '" stroke="#e0c050" stroke-width="1.5"/>';
  }

  /* a handle on a selected region: move(dx) while dragging, done(dx) after */
  function dragHandle(handle, move, done) {
    var startX = null;
    handle.addEventListener('pointerdown', function (e) {
      e.stopPropagation();
      startX = e.clientX;
      handle.setPointerCapture(e.pointerId);
    });
    handle.addEventListener('pointermove', function (e) {
      if (startX !== null) move(e.clientX - startX);
    });
    handle.addEventListener('pointerup', function (e) {
      if (startX === null) return;
      var dx = e.clientX - startX;
      startX = null;
      if (Math.abs(dx) >= 2) done(dx); else move(0);
    });
    handle.addEventListener('pointercancel', function () { startX = null; move(0); });
    handle.addEventListener('click', function (e) { e.stopPropagation(); });
  }

  function signedSamples(dx) { return pxToSamples(Math.abs(dx)) * (dx < 0 ? -1 : 1); }

  function attachHandles(div, r, width) {
    var left = samplesToPx(r.position);
    var minLen = Math.round(state.sampleRate * 0.01);
    var edit = function (method, params) {
      params.id = r.id;
      call(method, params).then(checked).then(refreshTimeline).catch(refreshTimeline);
    };

    var trimL = el('div', 'handle trim l');
    dragHandle(trimL, function (dx) {
      var d = Math.max(-samplesToPx(r.start), Math.min(dx, width - 8));
      div.style.left = (left + d) + 'px';
      div.style.width = (width - d) + 'px';
    }, function (dx) {
      var start = r.position + signedSamples(dx);
      start = Math.max(r.position - r.start, Math.min(start, r.position + r.length - minLen));
      edit('region.trim', { start: Math.max(0, start) });
    });
    div.appendChild(trimL);

    var trimR = el('div', 'handle trim r');
    dragHandle(trimR, function (dx) {
      div.style.width = Math.max(8, width + dx) + 'px';
    }, function (dx) {
      var end = r.position + r.length + signedSamples(dx);
      edit('region.trim', { end: Math.max(r.position + minLen, end) });
    });
    div.appendChild(trimR);

    if (r.fade_in === undefined) return;
    var svg = div.querySelector('svg.fades');
    var fadeIn = el('div', 'handle fade-h');
    fadeIn.style.left = Math.max(0, samplesToPx(r.fade_in) - 7) + 'px';
    dragHandle(fadeIn, function (dx) {
      var fi = Math.max(0, r.fade_in + signedSamples(dx));
      fadeIn.style.left = Math.max(0, samplesToPx(fi) - 7) + 'px';
      drawFades(svg, width, fi, r.fade_out);
    }, function (dx) {
      edit('region.fades', { fade_in: Math.max(64, r.fade_in + signedSamples(dx)) });
    });
    div.appendChild(fadeIn);

    var fadeOut = el('div', 'handle fade-h');
    fadeOut.style.right = Math.max(0, samplesToPx(r.fade_out) - 7) + 'px';
    dragHandle(fadeOut, function (dx) {
      var fo = Math.max(0, r.fade_out - signedSamples(dx));
      fadeOut.style.right = Math.max(0, samplesToPx(fo) - 7) + 'px';
      drawFades(svg, width, r.fade_in, fo);
    }, function (dx) {
      edit('region.fades', { fade_out: Math.max(64, r.fade_out - signedSamples(dx)) });
    });
    div.appendChild(fadeOut);
  }

  function drawPeaks(canvas, r) {
    var key = r.id + ':' + r.start + ':' + r.length + ':' + canvas.width;
    var paint = function (peaks) {
      var ctx = canvas.getContext('2d');
      var h = canvas.height, mid = h / 2;
      ctx.clearRect(0, 0, canvas.width, h);
      ctx.fillStyle = 'rgba(230,230,230,0.75)';
      var n = peaks.length / 2;
      var scale = canvas.width / Math.max(1, n);
      for (var i = 0; i < n; i++) {
        var lo = peaks[2 * i], hi = peaks[2 * i + 1];
        var y0 = mid - hi * mid, y1 = mid - lo * mid;
        ctx.fillRect(i * scale, y0, Math.max(1, scale), Math.max(1, y1 - y0));
      }
    };
    if (state.peaks[key]) { paint(state.peaks[key]); return; }
    call('region.peaks', { id: r.id, count: canvas.width }).then(function (p) {
      if (!p.ok) return;
      state.peaks[key] = p.peaks;
      paint(p.peaks);
    });
  }

  /* a MIDI region's notes, as a miniature piano roll */
  function drawNotes(canvas, r) {
    var paint = function (notes) {
      var ctx = canvas.getContext('2d');
      ctx.clearRect(0, 0, canvas.width, canvas.height);
      if (!notes.length) return;
      var lo = 127, hi = 0;
      notes.forEach(function (n) { lo = Math.min(lo, n.note); hi = Math.max(hi, n.note); });
      lo -= 2; hi += 2;
      var rows = hi - lo + 1, rowH = Math.max(1, Math.min(6, canvas.height / rows));
      var top = (canvas.height - rows * rowH) / 2;
      var perBeat = canvas.width / Math.max(0.001, r.length_beats);
      ctx.fillStyle = 'rgba(200,255,215,0.9)';
      notes.forEach(function (n) {
        var x = (n.start - r.start_beats) * perBeat;
        ctx.fillRect(x, top + (hi - n.note) * rowH, Math.max(2, n.length * perBeat - 1), Math.max(1, rowH - 1));
      });
    };
    var cached = state.notes[r.id];
    if (cached) { paint(cached); return; }
    call('region.notes', { id: r.id }).then(function (reply) {
      if (!reply.ok) return;
      state.notes[r.id] = reply.notes;
      paint(reply.notes);
    });
  }

  function attachDrag(div, track, r) {
    var start = null;
    div.addEventListener('pointerdown', function (e) {
      e.stopPropagation();
      start = { x: e.clientX, y: e.clientY, moved: false };
      div.setPointerCapture(e.pointerId);
    });
    div.addEventListener('pointermove', function (e) {
      if (!start) return;
      var dx = e.clientX - start.x, dy = e.clientY - start.y;
      if (!start.moved && Math.abs(dx) < 6 && Math.abs(dy) < 6) return;
      start.moved = true;
      div.classList.add('dragging');
      div.style.transform = 'translate(' + dx + 'px,' + dy + 'px)';
    });
    var finish = function (e) {
      if (!start) return;
      var s = start;
      start = null;
      div.classList.remove('dragging');
      div.style.transform = '';
      if (!s.moved) { selectRegion(r.id, track.id); return; }
      var dx = e.clientX - s.x;
      var newPos = Math.max(0, r.position + pxToSamples(Math.abs(dx)) * (dx < 0 ? -1 : 1));
      var params = { id: r.id, position: newPos };
      div.style.visibility = 'hidden';
      var target = document.elementFromPoint(e.clientX, e.clientY);
      div.style.visibility = '';
      var lane = target && target.closest ? target.closest('.lane') : null;
      if (lane && lane.dataset.track !== track.id) params.track = lane.dataset.track;
      state.selectedRegion = r.id;
      call('region.move', params).then(checked).then(refreshTimeline).catch(refreshTimeline);
    };
    div.addEventListener('pointerup', finish);
    div.addEventListener('pointercancel', function () {
      start = null;
      div.classList.remove('dragging');
      div.style.transform = '';
    });
  }

  function selectTrack(id) {
    state.selectedTrack = id;
    document.querySelectorAll('.track-head').forEach(function (h, i) {
      h.classList.toggle('selected', state.timeline.tracks[i] && state.timeline.tracks[i].id === id);
    });
    document.querySelectorAll('.lane').forEach(function (l) { l.classList.toggle('selected', l.dataset.track === id); });
    updateRegionTools();
  }

  function selectRegion(id, trackId) {
    state.selectedRegion = state.selectedRegion === id ? null : id;
    if (trackId) selectTrack(trackId);
    document.querySelectorAll('.region').forEach(function (d) {
      d.classList.toggle('selected', d.dataset.region === state.selectedRegion);
    });
    updateRegionTools();
  }

  function updateRegionTools() {
    var found = state.selectedRegion ? findRegion(state.selectedRegion) : null;
    $('split').disabled = !found;
    $('delete-region').disabled = !found;
    $('mute-region').disabled = !found;
    $('mute-region').textContent = found && found.region.muted ? 'Unmute' : 'Mute';
    var track = state.timeline && state.timeline.tracks.filter(function (t) { return t.id === state.selectedTrack; })[0];
    $('tone').disabled = !track || track.kind !== 'audio';
    $('midi-region').disabled = !track || track.kind !== 'midi';
    $('edit-notes').disabled = !found || found.region.kind !== 'midi';
    $('import').disabled = !track || track.kind !== 'audio';
    $('import').style.display = window.ArdourBridge.canPickFiles() ? '' : 'none';
  }

  $('add-track').onsubmit = function (e) {
    e.preventDefault();
    var kind = $('track-kind').value;
    var params = kind === 'midi'
      ? { kind: 'midi', name: $('track-name').value.trim() || 'MIDI' }
      : { name: $('track-name').value.trim() || 'Audio', channels: Number($('track-channels').value) };
    call('tracks.add', params).then(checked).then(function (r) {
      state.selectedTrack = r.id;
      return refreshAll();
    }).catch(ignore);
  };

  $('track-kind').onchange = function () {
    $('track-channels').style.display = $('track-kind').value === 'midi' ? 'none' : '';
  };

  $('midi-region').onclick = function () {
    if (!state.selectedTrack) return;
    call('regions.midi', { track: state.selectedTrack, position: state.position, beats: 8 })
      .then(checked).then(function (r) { state.selectedRegion = r.region.id; return refreshTimeline(); }).catch(ignore);
  };

  $('edit-notes').onclick = function () { if (state.selectedRegion) openRoll(state.selectedRegion); };

  $('tone').onclick = function () {
    if (!state.selectedTrack) return;
    call('regions.tone', { track: state.selectedTrack, position: state.position, seconds: 2, frequency: 440 })
      .then(checked).then(function (r) { state.selectedRegion = r.region.id; return refreshTimeline(); }).catch(ignore);
  };

  $('import').onclick = function () {
    var track = state.selectedTrack, at = state.position;
    if (!track) return;
    window.ArdourBridge.pickFile().then(function (path) {
      if (!path) return;
      message('importing…');
      return call('regions.import', { track: track, path: path, position: at }).then(checked).then(function (r) {
        state.selectedRegion = r.region.id;
        if (r.warning) message(r.warning);
        return refreshTimeline();
      });
    }).catch(ignore);
  };

  $('split').onclick = function () {
    if (!state.selectedRegion) return;
    call('region.split', { id: state.selectedRegion, position: state.position })
      .then(checked).then(function () { state.selectedRegion = null; return refreshTimeline(); }).catch(ignore);
  };

  $('mute-region').onclick = function () {
    var found = state.selectedRegion ? findRegion(state.selectedRegion) : null;
    if (!found) return;
    call('region.mute', { id: found.region.id, muted: !found.region.muted }).then(checked).then(refreshTimeline).catch(ignore);
  };

  $('delete-region').onclick = function () {
    if (!state.selectedRegion) return;
    call('region.remove', { id: state.selectedRegion })
      .then(checked).then(function () { state.selectedRegion = null; return refreshTimeline(); }).catch(ignore);
  };

  function zoom(factor) {
    var scroll = $('lanes-scroll');
    var centre = (scroll.scrollLeft + scroll.clientWidth / 2) / state.pxPerSec;
    state.pxPerSec = Math.max(2, Math.min(800, state.pxPerSec * factor));
    renderTimeline();
    scroll.scrollLeft = Math.max(0, centre * state.pxPerSec - scroll.clientWidth / 2);
  }
  $('zoom-in').onclick = function () { zoom(2); };
  $('zoom-out').onclick = function () { zoom(0.5); };

  /* keep the track heads level with the lanes when the lanes scroll */
  $('lanes-scroll').addEventListener('scroll', function () {
    $('track-heads').scrollTop = $('lanes-scroll').scrollTop;
  });

  /* ----------------------------------------------------------------- mixer */

  function refreshStrips() {
    if (!state.session) return Promise.resolve();
    return call('mixer.strips').then(function (r) {
      if (!r.ok) return;
      var sameShape = r.strips.length === state.strips.length && r.strips.every(function (s, i) {
        return state.strips[i].id === s.id && state.strips[i].name === s.name &&
          pluginKey(state.strips[i]) === pluginKey(s);
      });
      state.strips = r.strips;
      if (sameShape && $('strips').children.length) {
        r.strips.forEach(updateStrip);
      } else {
        renderStrips();
      }
    });
  }

  function pluginKey(s) {
    return (s.plugins || []).map(function (p) { return p.id + (p.active ? '+' : '-'); }).join(',');
  }

  function renderStrips() {
    var box = $('strips');
    box.innerHTML = '';
    state.strips.forEach(function (s) { box.appendChild(stripElement(s)); });
  }

  function setStrip(id, params) {
    params.id = id;
    return call('strip.set', params).then(checked).then(function (r) {
      for (var i = 0; i < state.strips.length; i++) if (state.strips[i].id === id) state.strips[i] = r.strip;
      updateStrip(r.strip);
      /* solo changes other strips' implicit state */
      if ('solo' in params) refreshStrips();
    }).catch(ignore);
  }

  /* a fader request at a time; the latest value wins */
  function throttled(fn) {
    var busy = false, next = null;
    function run(v) {
      busy = true;
      fn(v).then(function () {
        busy = false;
        if (next !== null) { var n = next; next = null; run(n); }
      });
    }
    return function (v) { if (busy) next = v; else run(v); };
  }

  function stripElement(s) {
    var div = el('div', 'strip ' + s.kind);
    div.dataset.strip = s.id;

    div.appendChild(el('div', 'strip-name', s.name));

    var pan = el('input', 'pan');
    pan.type = 'range'; pan.min = -100; pan.max = 100; pan.step = 1;
    pan.disabled = s.pan === null;
    var panLabel = el('div', 'pan-label');
    var sendPan = throttled(function (v) { return setStrip(s.id, { pan: v / 100 }); });
    pan.oninput = function () { panLabel.textContent = formatPan(pan.value / 100); sendPan(Number(pan.value)); };
    pan.ondblclick = function () { pan.value = 0; pan.oninput(); };
    div.appendChild(pan);
    div.appendChild(panLabel);

    var body = el('div', 'strip-body');
    var faderBox = el('div', 'fader-box');
    var fader = el('input', 'fader');
    fader.type = 'range'; fader.min = -60; fader.max = 6; fader.step = 0.1;
    var gainLabel = el('div', 'gain-label');
    var sendGain = throttled(function (v) { return setStrip(s.id, { gain_db: v }); });
    fader.oninput = function () { gainLabel.textContent = formatGain(Number(fader.value)); sendGain(Number(fader.value)); };
    fader.ondblclick = function () { fader.value = 0; fader.oninput(); };
    faderBox.appendChild(fader);
    body.appendChild(faderBox);

    var meters = el('div', 'meters v');
    for (var i = 0; i < s.channels; i++) {
      var m = el('div', 'meter v');
      m.dataset.meter = s.id;
      m.dataset.channel = i;
      m.appendChild(el('div'));
      meters.appendChild(m);
    }
    body.appendChild(meters);
    div.appendChild(body);
    div.appendChild(gainLabel);

    var plugs = el('div', 'strip-plugins');
    (s.plugins || []).forEach(function (pl) {
      var b = el('button', (pl.active ? '' : 'bypassed') + (pl.instrument ? ' instrument' : ''), pl.name);
      b.title = pl.name + (pl.active ? '' : ' (bypassed)');
      b.onclick = function () { openPlugin(s.id, pl.id); };
      plugs.appendChild(b);
    });
    var addFx = el('button', 'add-fx', '+ FX');
    addFx.title = 'Add a plugin';
    addFx.onclick = function () { openPluginPicker(s.id, s.kind); };
    plugs.appendChild(addFx);
    div.appendChild(plugs);

    var buttons = el('div', 'strip-buttons');
    [['mute', 'M'], ['solo', 'S'], ['rec', 'R']].forEach(function (b) {
      var btn = el('button', 'toggle ' + b[0], b[1]);
      btn.dataset.control = b[0];
      btn.onclick = function () {
        var cur = state.strips.filter(function (x) { return x.id === s.id; })[0];
        var p = {}; p[b[0]] = !cur[b[0]];
        setStrip(s.id, p);
      };
      buttons.appendChild(btn);
    });
    div.appendChild(buttons);

    if (s.kind !== 'master') {
      var remove = el('button', 'remove', '✕');
      remove.title = 'Remove';
      remove.onclick = function () {
        if (!window.confirm('Remove ' + s.name + '?')) return;
        call('tracks.remove', { id: s.id }).then(checked).then(refreshAll).catch(ignore);
      };
      div.appendChild(remove);
    }

    updateStrip(s, div);
    return div;
  }

  function updateStrip(s, div) {
    div = div || document.querySelector('.strip[data-strip="' + s.id + '"]');
    if (!div) return;
    var fader = div.querySelector('.fader');
    if (document.activeElement !== fader) fader.value = Math.max(-60, s.gain_db);
    div.querySelector('.gain-label').textContent = formatGain(s.gain_db);
    var pan = div.querySelector('.pan');
    if (s.pan !== null && document.activeElement !== pan) pan.value = Math.round(s.pan * 100);
    div.querySelector('.pan-label').textContent = s.pan === null ? '' : formatPan(s.pan);
    div.querySelectorAll('.toggle').forEach(function (b) {
      var v = s[b.dataset.control];
      b.disabled = v === null || (b.dataset.control === 'solo' && s.kind === 'master');
      b.classList.toggle('on', !!v);
    });
  }

  $('add-bus').onclick = function () {
    call('buses.add', { name: 'Bus' }).then(checked).then(refreshStrips).catch(ignore);
  };

  /* --------------------------------------------------------------- polling */

  var lastStripRefresh = 0;
  var lastStatus = 0;

  /* follow a session opened or closed by someone else (the host, a test) */
  function watchStatus() {
    return call('engine.status').then(function (st) {
      if (!st.ok) return;
      var was = state.session ? state.session.path : null;
      var now = st.session ? st.session.path : null;
      if (was !== now) return afterSessionChange(st);
    });
  }

  function poll() {
    var now = Date.now();
    var status = Promise.resolve();
    if (now - lastStatus > 1000) {
      lastStatus = now;
      status = watchStatus();
    }
    if (!state.session) return status;
    return Promise.all([call('transport.state'), call('meters')]).then(function (r) {
      var t = r[0], m = r[1];
      if (t.ok) updateTransport(t);
      if (m.ok) {
        m.meters.forEach(function (meter) {
          var bars = document.querySelectorAll('[data-meter="' + meter.id + '"]');
          bars.forEach(function (bar) {
            var ch = bar.dataset.channel;
            var db = ch === undefined ? Math.max.apply(null, meter.peak.length ? meter.peak : [-200]) : meter.peak[Number(ch)];
            if (db === undefined) db = -200;
            var f = dbToFraction(db) * 100 + '%';
            if (bar.classList.contains('v')) bar.firstChild.style.height = f;
            else bar.firstChild.style.width = f;
          });
        });
      }
      var now = Date.now();
      if (state.view === 'mixer' && now - lastStripRefresh > 1000) {
        lastStripRefresh = now;
        refreshStrips();
      }
    });
  }

  /* wait for the engine, then keep the transport and the meters fresh */
  var polling = false;
  function pollLoop() {
    if (polling) return;
    polling = true;
    poll().then(function () { polling = false; }, function () { polling = false; });
  }

  /* --------------------------------------------------------------- overlay */

  function openOverlay(o, title) {
    state.overlay = o;
    $('overlay-title').textContent = title;
    $('overlay-tools').innerHTML = '';
    $('overlay-body').innerHTML = '';
    $('overlay').hidden = false;
  }

  function closeOverlay() {
    var was = state.overlay;
    state.overlay = null;
    $('overlay').hidden = true;
    $('overlay-body').innerHTML = '';
    if (was && was.kind === 'roll') { delete state.notes[was.region]; refreshTimeline(); }
    if (was && was.kind !== 'roll') refreshStrips();
  }
  $('overlay-close').onclick = closeOverlay;

  function tool(label, fn, title) {
    var b = el('button', '', label);
    if (title) b.title = title;
    b.onclick = fn;
    $('overlay-tools').appendChild(b);
    return b;
  }

  /* ------------------------------------------------------------ piano roll */

  var ROLL_ROW = 14, ROLL_TOP = 108, ROLL_BOTTOM = 24;
  var NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
  function isBlack(n) { return [1, 3, 6, 8, 10].indexOf(n % 12) >= 0; }
  function noteName(n) { return NOTE_NAMES[n % 12] + (Math.floor(n / 12) - 1); }

  function openRoll(regionId) {
    var found = findRegion(regionId);
    if (!found) return;
    openOverlay({ kind: 'roll', region: regionId, selected: null, snap: 0.25, length: 1, beatPx: 48, notes: [] },
                found.region.name);
    var o = state.overlay;
    var snap = el('select');
    [['1/8', 0.125], ['1/4', 0.25], ['1/2', 0.5], ['1', 1]].forEach(function (v) {
      var opt = el('option', '', 'snap ' + v[0]); opt.value = v[1]; if (v[1] === o.snap) opt.selected = true; snap.appendChild(opt);
    });
    snap.onchange = function () { o.snap = Number(snap.value); };
    $('overlay-tools').appendChild(snap);
    tool('−', function () { o.beatPx = Math.max(12, o.beatPx / 1.5); renderRoll(); }, 'Zoom out');
    tool('+', function () { o.beatPx = Math.min(200, o.beatPx * 1.5); renderRoll(); }, 'Zoom in');
    o.deleteButton = tool('Delete', function () {
      if (o.selected === null) return;
      call('region.note.remove', { id: o.region, note_id: o.selected }).then(checked).then(function () {
        o.selected = null; return loadRoll();
      }).catch(ignore);
    }, 'Delete the selected note');
    tool('↶', function () { call('edit.undo').then(checked).then(afterHistory).catch(ignore); }, 'Undo');
    var vel = el('input', 'velocity');
    vel.type = 'range'; vel.min = 1; vel.max = 127; vel.step = 1; vel.value = 100; vel.disabled = true;
    vel.title = 'Velocity of the selected note';
    var velLabel = el('span', 'vel-label', 'vel');
    vel.oninput = function () { velLabel.textContent = 'vel ' + vel.value; };
    vel.onchange = function () {
      if (o.selected === null) return;
      call('region.note.edit', { id: o.region, note_id: o.selected, velocity: Number(vel.value) })
        .then(checked).then(function () { return loadRoll(); }).catch(ignore);
    };
    $('overlay-tools').appendChild(velLabel);
    $('overlay-tools').appendChild(vel);
    o.velocity = vel; o.velocityLabel = velLabel;
    loadRoll(true);
  }

  function loadRoll(scroll) {
    var o = state.overlay;
    if (!o || o.kind !== 'roll') return Promise.resolve();
    return call('region.notes', { id: o.region }).then(checked).then(function (r) {
      if (state.overlay !== o) return;
      o.notes = r.notes;
      o.info = r.region;
      state.notes[o.region] = r.notes;
      renderRoll();
      if (scroll) {
        var sum = 0; r.notes.forEach(function (n) { sum += n.note; });
        var centre = r.notes.length ? sum / r.notes.length : 60;
        var body = $('overlay-body');
        var first = r.notes.length ? Math.min.apply(null, r.notes.map(function (n) { return n.start; })) : r.region.start_beats;
        requestAnimationFrame(function () {
          body.scrollTop = Math.max(0, (ROLL_TOP - centre) * ROLL_ROW - body.clientHeight / 2);
          body.scrollLeft = Math.max(0, (first - r.region.start_beats) * o.beatPx - 20);
        });
      }
    }).catch(ignore);
  }

  function renderRoll() {
    var o = state.overlay;
    if (!o || o.kind !== 'roll' || !o.info) return;
    var body = $('overlay-body');
    var keepX = body.scrollLeft, keepY = body.scrollTop;
    body.innerHTML = '';
    var beats = o.info.length_beats, sb = o.info.start_beats;
    var width = Math.max(1, beats * o.beatPx);
    var roll = el('div', 'roll');
    var keys = el('div', 'roll-keys');
    var grid = el('div', 'roll-grid');
    grid.style.width = width + 'px';
    for (var n = ROLL_TOP; n >= ROLL_BOTTOM; n--) {
      keys.appendChild(el('div', 'roll-key' + (isBlack(n) ? ' black' : ''), n % 12 === 0 ? noteName(n) : ''));
      grid.appendChild(el('div', 'roll-row' + (isBlack(n) ? ' black' : '')));
    }
    for (var b = 0; b <= beats; b++) {
      var line = el('div', 'roll-beat' + (b % 4 === 0 ? ' bar' : ''));
      line.style.left = (b * o.beatPx) + 'px';
      grid.appendChild(line);
    }
    var yOf = function (note) { return (ROLL_TOP - note) * ROLL_ROW; };
    o.notes.forEach(function (nt) {
      var d = el('div', 'roll-note' + (nt.id === o.selected ? ' selected' : ''));
      d.style.left = ((nt.start - sb) * o.beatPx) + 'px';
      d.style.top = yOf(nt.note) + 'px';
      d.style.width = Math.max(4, nt.length * o.beatPx) + 'px';
      d.title = noteName(nt.note) + ' velocity ' + nt.velocity;
      d.style.opacity = 0.35 + 0.65 * nt.velocity / 127;
      attachNoteDrag(d, nt);
      grid.appendChild(d);
    });
    grid.addEventListener('click', function (e) {
      if (e.target.classList.contains('roll-note')) return;
      var rect = grid.getBoundingClientRect();
      var x = e.clientX - rect.left, y = e.clientY - rect.top;
      var pitch = ROLL_TOP - Math.floor(y / ROLL_ROW);
      var start = Math.floor(x / o.beatPx / o.snap) * o.snap;
      if (start >= beats) return;
      var len = Math.min(o.length, beats - start);
      call('region.note.add', { id: o.region, note: pitch, start: sb + start, length: len, velocity: 100 })
        .then(checked).then(function (r) { o.selected = r.id; return loadRoll(); }).catch(ignore);
    });
    roll.appendChild(keys);
    roll.appendChild(grid);
    body.appendChild(roll);
    body.scrollLeft = keepX; body.scrollTop = keepY;
    o.deleteButton.disabled = o.selected === null;
    var sel = o.notes.filter(function (x) { return x.id === o.selected; })[0];
    o.velocity.disabled = !sel;
    if (sel) { o.velocity.value = sel.velocity; o.velocityLabel.textContent = 'vel ' + sel.velocity; }
    else o.velocityLabel.textContent = 'vel';
  }

  /* drag a note to move it in time and pitch; drag its right end to resize */
  function attachNoteDrag(d, nt) {
    var drag = null;
    d.addEventListener('pointerdown', function (e) {
      e.stopPropagation();
      var rect = d.getBoundingClientRect();
      drag = { x: e.clientX, y: e.clientY, resize: e.clientX > rect.right - 10, moved: false };
      d.setPointerCapture(e.pointerId);
    });
    d.addEventListener('pointermove', function (e) {
      if (!drag) return;
      var o = state.overlay;
      var dx = e.clientX - drag.x, dy = e.clientY - drag.y;
      if (Math.abs(dx) + Math.abs(dy) > 4) drag.moved = true;
      if (!drag.moved) return;
      if (drag.resize) {
        d.style.width = Math.max(4, nt.length * o.beatPx + dx) + 'px';
      } else {
        d.style.transform = 'translate(' + dx + 'px,' + (Math.round(dy / ROLL_ROW) * ROLL_ROW) + 'px)';
      }
    });
    var end = function (e) {
      if (!drag) return;
      var o = state.overlay, g = drag;
      drag = null;
      if (!g.moved) {
        o.selected = o.selected === nt.id ? null : nt.id;
        renderRoll();
        return;
      }
      var dx = e.clientX - g.x, dy = e.clientY - g.y;
      var params = { id: o.region, note_id: nt.id };
      if (g.resize) {
        params.length = Math.max(o.snap, Math.round((nt.length + dx / o.beatPx) / o.snap) * o.snap);
        o.length = params.length;
      } else {
        params.start = Math.max(o.info.start_beats, nt.start + Math.round(dx / o.beatPx / o.snap) * o.snap);
        params.note = Math.max(0, Math.min(127, nt.note - Math.round(dy / ROLL_ROW)));
      }
      o.selected = nt.id;
      call('region.note.edit', params).then(checked).then(function () { return loadRoll(); }).catch(loadRoll);
    };
    d.addEventListener('pointerup', end);
    d.addEventListener('pointercancel', function () { drag = null; renderRoll(); });
    d.addEventListener('click', function (e) { e.stopPropagation(); });
  }

  /* --------------------------------------------------------------- plugins */

  function openPluginPicker(routeId, kind) {
    var strip = state.strips.filter(function (x) { return x.id === routeId; })[0];
    openOverlay({ kind: 'plugins', route: routeId }, 'Add a plugin to ' + (strip ? strip.name : ''));
    call('plugins.list').then(checked).then(function (r) {
      var o = state.overlay;
      if (!o || o.kind !== 'plugins') return;
      var list = el('ul', 'plugin-list');
      var sorted = r.plugins.slice().sort(function (a, b) {
        return (a.instrument - b.instrument) || a.name.localeCompare(b.name);
      });
      sorted.forEach(function (pl) {
        if (pl.instrument && kind !== 'midi') return;
        var li = el('li');
        var label = el('span', '', pl.name + ' ');
        label.appendChild(el('small', '', pl.type.toUpperCase() + (pl.category ? ' · ' + pl.category : '')));
        li.appendChild(label);
        var add = el('button', '', 'Add');
        add.onclick = function () {
          call('plugin.add', { route: routeId, id: pl.id }).then(checked).then(function (reply) {
            refreshStrips();
            openPlugin(routeId, reply.plugin.id, reply.plugin);
          }).catch(ignore);
        };
        li.appendChild(add);
        list.appendChild(li);
      });
      $('overlay-body').appendChild(list);
    }).catch(ignore);
  }

  function formatParam(pm) {
    if (pm.text) return pm.text;
    return pm.integer ? String(Math.round(pm.value)) : Number(pm.value).toFixed(2);
  }

  /* the slider position (0..1000) for a parameter value, and back */
  function paramToSlider(pm, v) {
    if (pm.log && pm.min > 0) return Math.round(1000 * Math.log(v / pm.min) / Math.log(pm.max / pm.min));
    return Math.round(1000 * (v - pm.min) / (pm.max - pm.min || 1));
  }
  function sliderToParam(pm, x) {
    var f = x / 1000;
    var v = pm.log && pm.min > 0 ? pm.min * Math.pow(pm.max / pm.min, f) : pm.min + f * (pm.max - pm.min);
    return pm.integer ? Math.round(v) : v;
  }

  function openPlugin(routeId, pluginId, known) {
    openOverlay({ kind: 'plugin', route: routeId, plugin: pluginId }, '');
    var load = known ? Promise.resolve({ ok: true, plugin: known }) : call('plugin.get', { route: routeId, plugin: pluginId });
    load.then(checked).then(function (r) { renderPlugin(r.plugin); }).catch(ignore);
  }

  function renderPlugin(pl) {
    var o = state.overlay;
    if (!o || o.kind !== 'plugin') return;
    o.info = pl;
    $('overlay-title').textContent = pl.name;
    $('overlay-tools').innerHTML = '';
    var send = function (params) {
      params.route = o.route; params.plugin = o.plugin;
      return call('plugin.set', params).then(checked);
    };
    var bypass = tool(pl.active ? 'Bypass' : 'Enable', function () {
      send({ active: !o.info.active }).then(function (r) { renderPlugin(r.plugin); }).catch(ignore);
    });
    bypass.classList.toggle('on', !pl.active);
    tool('Remove', function () {
      call('plugin.remove', { route: o.route, plugin: o.plugin }).then(checked).then(closeOverlay).catch(ignore);
    });
    var presets = el('select', 'presets');
    presets.appendChild(el('option', '', 'Presets…'));
    $('overlay-tools').appendChild(presets);
    call('plugin.presets', { route: o.route, plugin: o.plugin }).then(function (r) {
      if (!r.ok) return;
      r.presets.forEach(function (pr) {
        var opt = el('option', '', pr.label + (pr.user ? ' (user)' : ''));
        opt.value = pr.uri;
        if (pr.uri === r.current) opt.selected = true;
        presets.appendChild(opt);
      });
    });
    presets.onchange = function () {
      if (!presets.value || presets.selectedIndex === 0) return;
      call('plugin.preset.load', { route: o.route, plugin: o.plugin, uri: presets.value })
        .then(checked).then(function (r) { renderPlugin(r.plugin); }).catch(ignore);
    };
    tool('Save…', function () {
      var name = window.prompt('Preset name');
      if (!name) return;
      call('plugin.preset.save', { route: o.route, plugin: o.plugin, name: name })
        .then(checked).then(function () { renderPlugin(o.info); message('preset saved'); }).catch(ignore);
    }, 'Save the current settings as a preset');
    var body = $('overlay-body');
    body.innerHTML = '';
    var grid = el('div', 'plugin-params');
    if (!pl.params.length) grid.appendChild(el('label', '', 'This plugin has no controls.'));
    pl.params.forEach(function (pm) {
      grid.appendChild(el('label', '', pm.name));
      var input = el('input');
      var val = el('span', 'val', formatParam(pm));
      if (pm.toggled) {
        input.type = 'checkbox';
        input.checked = pm.value > (pm.min + pm.max) / 2;
        input.onchange = function () {
          send({ param: pm.index, value: input.checked ? pm.max : pm.min }).then(function (r) { updateParams(r.plugin); }).catch(ignore);
        };
      } else {
        input.type = 'range'; input.min = 0; input.max = 1000; input.step = 1;
        input.value = paramToSlider(pm, pm.value);
        var push = throttled(function (x) {
          return send({ param: pm.index, value: sliderToParam(pm, x) }).then(function (r) { updateParams(r.plugin); }, ignore);
        });
        input.oninput = function () { push(Number(input.value)); };
      }
      input.dataset.param = pm.index;
      val.dataset.param = pm.index;
      grid.appendChild(input);
      grid.appendChild(val);
    });
    body.appendChild(grid);
  }

  function updateParams(pl) {
    var o = state.overlay;
    if (!o || o.kind !== 'plugin') return;
    o.info = pl;
    pl.params.forEach(function (pm) {
      var v = document.querySelector('.plugin-params .val[data-param="' + pm.index + '"]');
      if (v) v.textContent = formatParam(pm);
    });
  }

  /* for hosts and tests: switch views, and describe what is drawn */
  window.ArdourApp = {
    show: function (name) {
      if (!state.session && name !== 'session') return;
      if (name === 'automation') {
        if (state.overlay) closeOverlay();
        showView('editor');
        (state.timeline ? state.timeline.tracks : []).forEach(function (t) { state.automation[t.id] = { control: 'gain' }; });
        state.keyboard = true;
        renderTimeline();
        renderKeyboard();
        return;
      }
      if (name === 'roll' || name === 'plugin') {
        var strips = state.strips;
        if (name === 'plugin') {
          for (var i = 0; i < strips.length; i++) {
            var fx = (strips[i].plugins || []).filter(function (p) { return !p.instrument; })[0];
            if (fx) { showView('mixer'); openPlugin(strips[i].id, fx.id); return; }
          }
          return;
        }
        var tracks = state.timeline ? state.timeline.tracks : [];
        for (var t = 0; t < tracks.length; t++) {
          var mr = tracks[t].regions.filter(function (r) { return r.kind === 'midi'; })[0];
          if (mr) { showView('editor'); selectRegion(mr.id, tracks[t].id); openRoll(mr.id); return; }
        }
        return;
      }
      if (state.overlay) closeOverlay();
      showView(name);
    },
    refresh: function () { return refreshAll(); },
    selectLastRegion: function () {
      var all = document.querySelectorAll('.region');
      if (!all.length) return;
      var d = all[all.length - 1];
      var lane = d.closest('.lane');
      selectRegion(d.dataset.region, lane ? lane.dataset.track : null);
    },
    describe: function () {
      return 'view=' + state.view +
        ' session=' + (state.session ? state.session.name : '-') +
        ' lanes=' + document.querySelectorAll('.lane').length +
        ' regions=' + document.querySelectorAll('.region').length +
        ' strips=' + document.querySelectorAll('.strip').length +
        ' selected=' + (state.selectedRegion ? 1 : 0) +
        ' undo=' + ($('undo').disabled ? 0 : 1) +
        ' overlay=' + (state.overlay ? state.overlay.kind : '-') +
        ' notes=' + document.querySelectorAll('.roll-note').length +
        ' params=' + document.querySelectorAll('.plugin-params input').length +
        ' fx=' + document.querySelectorAll('.strip-plugins button:not(.add-fx)').length +
        ' autopoints=' + document.querySelectorAll('.auto-point').length +
        ' keys=' + document.querySelectorAll('#keyboard:not([hidden]) .kb-key').length;
    }
  };

  function boot() {
    refreshStatus().then(function (st) {
      if (st.ok && st.initialized === false) { setTimeout(boot, 300); return; }
      if (!st.ok) return;
      refreshSessions();
      if (state.session) { showView('editor'); refreshAll(); }
      setInterval(pollLoop, 100);
    });
  }
  boot();
})();
