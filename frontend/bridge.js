/*
 * The engine bridge of the shared frontend.
 *
 * One call: ArdourBridge.call(method, params) -> Promise of the reply object.
 * The methods and replies are those of libs/android/engine_protocol.h; this
 * file only knows how each host carries them:
 *
 *   Android  window.ArdourNative.call(method, json) -> json  (JavascriptInterface)
 *   iOS      window.webkit.messageHandlers.ardour.postMessage({id, method, params}),
 *            answered by the host with window.ArdourBridge.reply(id, json)
 *   browser  window.ArdourWasm.call(method, params) -> Promise (the wasm engine)
 */
(function () {
  'use strict';

  var pending = {};
  var nextId = 1;

  function parse(text) {
    try { return JSON.parse(text); }
    catch (e) { return { ok: false, error: 'bad reply from the engine: ' + text }; }
  }

  function host() {
    if (window.ArdourNative && typeof window.ArdourNative.call === 'function') return 'android';
    if (window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.ardour) return 'ios';
    if (window.ArdourWasm && typeof window.ArdourWasm.call === 'function') return 'wasm';
    return 'none';
  }

  function call(method, params) {
    var json = JSON.stringify(params || {});
    switch (host()) {
      case 'android':
        return Promise.resolve(parse(window.ArdourNative.call(method, json)));
      case 'ios':
        return new Promise(function (resolve) {
          var id = nextId++;
          pending[id] = resolve;
          window.webkit.messageHandlers.ardour.postMessage({ id: id, method: method, params: json });
        });
      case 'wasm':
        return Promise.resolve(window.ArdourWasm.call(method, params || {}));
      default:
        return Promise.resolve({ ok: false, error: 'no engine host' });
    }
  }

  function reply(id, text) {
    var resolve = pending[id];
    if (resolve) {
      delete pending[id];
      resolve(typeof text === 'string' ? parse(text) : text);
    }
  }

  window.ArdourBridge = { call: call, reply: reply, host: host };
})();
