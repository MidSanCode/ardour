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
    peaks: {}
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

  $('undo').onclick = function () { call('edit.undo').then(checked).then(refreshTimeline).catch(ignore); };
  $('redo').onclick = function () { call('edit.redo').then(checked).then(refreshTimeline).catch(ignore); };

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
    $('lanes').style.height = (RULER_HEIGHT + tl.tracks.length * LANE_HEIGHT) + 'px';
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
    });
    $('playhead').style.left = samplesToPx(state.position) + 'px';
    updateRegionTools();
  }

  function regionElement(track, r) {
    var div = el('div', 'region' + (r.muted ? ' muted' : '') + (r.id === state.selectedRegion ? ' selected' : ''));
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
    drawPeaks(canvas, r);
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
    $('import').disabled = !track || track.kind !== 'audio';
    $('import').style.display = window.ArdourBridge.canPickFiles() ? '' : 'none';
  }

  $('add-track').onsubmit = function (e) {
    e.preventDefault();
    var params = { name: $('track-name').value.trim() || 'Audio', channels: Number($('track-channels').value) };
    call('tracks.add', params).then(checked).then(function (r) {
      state.selectedTrack = r.id;
      return refreshAll();
    }).catch(ignore);
  };

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
        return state.strips[i].id === s.id && state.strips[i].name === s.name;
      });
      state.strips = r.strips;
      if (sameShape && $('strips').children.length) {
        r.strips.forEach(updateStrip);
      } else {
        renderStrips();
      }
    });
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

  /* for hosts and tests: switch views, and describe what is drawn */
  window.ArdourApp = {
    show: function (name) { if (state.session || name === 'session') showView(name); },
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
        ' undo=' + ($('undo').disabled ? 0 : 1);
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
