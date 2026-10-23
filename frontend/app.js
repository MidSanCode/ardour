/*
 * The shared Ardour frontend: sessions, transport, tracks and meters over
 * ArdourBridge (bridge.js).  Plain JavaScript, no build step, so that the same
 * files run in the Android WebView, the iOS WKWebView and a browser.
 */
(function () {
  'use strict';

  var $ = function (id) { return document.getElementById(id); };
  var call = window.ArdourBridge.call;
  var state = { running: false, session: null, rolling: false, tracks: [] };

  function message(text) { $('message').textContent = text || ''; }

  function checked(reply) {
    if (!reply || !reply.ok) {
      message(reply && reply.error ? reply.error : 'request failed');
      throw new Error(reply && reply.error);
    }
    message('');
    return reply;
  }

  function dbToWidth(db) {
    /* -60 dBFS .. 0 dBFS onto 0 .. 100 % */
    var w = (db + 60) / 60 * 100;
    return Math.max(0, Math.min(100, w));
  }

  function formatClock(seconds) {
    var ms = Math.floor((seconds % 1) * 1000);
    var s = Math.floor(seconds) % 60;
    var m = Math.floor(seconds / 60) % 60;
    var h = Math.floor(seconds / 3600);
    function pad(n, w) { n = String(n); while (n.length < w) n = '0' + n; return n; }
    return pad(h, 2) + ':' + pad(m, 2) + ':' + pad(s, 2) + '.' + pad(ms, 3);
  }

  function applyStatus(st) {
    state.running = !!st.running;
    state.session = st.session || null;
    var badge = $('engine');
    if (st.starting) {
      badge.textContent = 'engine: starting…';
      badge.className = 'badge';
    } else if (st.running) {
      badge.textContent = 'engine: ' + st.backend + ' · ' + st.sample_rate + ' Hz · ' + st.buffer_size;
      badge.className = 'badge ok';
    } else {
      badge.textContent = 'engine: stopped';
      badge.className = 'badge err';
    }
    $('current').textContent = state.session ? state.session.name : 'no session loaded';
    var haveSession = !!state.session;
    ['save', 'close', 'play', 'stop'].forEach(function (id) { $(id).disabled = !haveSession; });
    $('add-track').querySelector('button').disabled = !haveSession;
  }

  function refreshStatus() {
    return call('engine.status').then(function (st) {
      if (!st.ok) {
        $('engine').textContent = 'engine: ' + st.error;
        $('engine').className = 'badge err';
        return st;
      }
      applyStatus(st);
      return st;
    });
  }

  function refreshSessions() {
    return call('session.list').then(function (r) {
      var ul = $('session-list');
      ul.innerHTML = '';
      if (!r.ok) return;
      r.sessions.forEach(function (name) {
        var li = document.createElement('li');
        var label = document.createElement('span');
        label.textContent = name;
        var open = document.createElement('button');
        open.textContent = 'Open';
        open.onclick = function () {
          call('session.open', { name: name }).then(checked).then(applyStatus).then(refreshTracks).catch(function () {});
        };
        li.appendChild(label);
        li.appendChild(open);
        ul.appendChild(li);
      });
    });
  }

  function refreshTracks() {
    var ul = $('track-list');
    if (!state.session) { ul.innerHTML = ''; state.tracks = []; return Promise.resolve(); }
    return call('tracks.list').then(function (r) {
      if (!r.ok) return;
      state.tracks = r.tracks;
      ul.innerHTML = '';
      r.tracks.forEach(function (t) {
        var li = document.createElement('li');
        li.className = 'track';
        li.dataset.id = t.id;
        var name = document.createElement('span');
        name.textContent = t.name + (t.inputs === 2 ? ' (stereo)' : ' (mono)');
        var meters = document.createElement('div');
        meters.className = 'meters';
        var n = Math.max(1, t.outputs);
        for (var i = 0; i < n; i++) {
          var m = document.createElement('div');
          m.className = 'meter';
          m.appendChild(document.createElement('div'));
          meters.appendChild(m);
        }
        li.appendChild(name);
        li.appendChild(meters);
        ul.appendChild(li);
      });
    });
  }

  function poll() {
    if (!state.session) return Promise.resolve();
    return Promise.all([call('transport.state'), call('meters')]).then(function (r) {
      var t = r[0], m = r[1];
      if (t.ok) {
        state.rolling = t.rolling;
        $('clock').textContent = formatClock(t.seconds);
        $('play').className = t.rolling ? 'rolling' : '';
      }
      if (m.ok) {
        m.meters.forEach(function (meter) {
          var li = document.querySelector('.track[data-id="' + meter.id + '"]');
          if (!li) return;
          var bars = li.querySelectorAll('.meter > div');
          meter.peak.forEach(function (db, i) { if (bars[i]) bars[i].style.width = dbToWidth(db) + '%'; });
        });
      }
    });
  }

  $('create').onsubmit = function (e) {
    e.preventDefault();
    var name = $('new-name').value.trim();
    if (!name) return;
    call('session.create', { name: name }).then(checked).then(function (st) {
      $('new-name').value = '';
      applyStatus(st);
      return Promise.all([refreshSessions(), refreshTracks()]);
    }).catch(function () {});
  };

  $('save').onclick = function () { call('session.save').then(checked).catch(function () {}); };
  $('close').onclick = function () {
    call('session.close').then(checked).then(applyStatus).then(refreshTracks).catch(function () {});
  };
  $('play').onclick = function () { call('transport.play').then(checked).catch(function () {}); };
  $('stop').onclick = function () { call('transport.stop').then(checked).catch(function () {}); };

  $('add-track').onsubmit = function (e) {
    e.preventDefault();
    var params = { name: $('track-name').value.trim() || 'Audio', channels: Number($('track-channels').value) };
    call('tracks.add', params).then(checked).then(refreshTracks).catch(function () {});
  };

  /* wait for the engine, then keep the transport and the meters fresh */
  function boot() {
    refreshStatus().then(function (st) {
      if (st.ok && st.initialized === false) { setTimeout(boot, 300); return; }
      if (!st.ok) return;
      refreshSessions();
      refreshTracks();
      setInterval(poll, 100);
    });
  }
  boot();
})();
