/* ui.js — qzos JSON UI 桥的 JS 封装（宿主 bridge.c 对端）
 *
 * 协议（见 os/src/bridge.c 顶部注释）：
 *   postMessage({op:'create'|'set'|'del'|'on'|'clear'|'focus'|'refresh'|'rpc', ...})
 *   宿主事件: {evt:'click'|'value'|'key'|'rpc'|'error'|'ready', ...}
 *
 * e-ink 约束：本层不做任何定时器/动画，事件驱动 only。
 */
(function (global) {
  'use strict';

  var handlers = {};   /* "<id>:<event>" -> fn */
  var keyHandlers = [];
  var rpcPending = {}; /* rid -> {resolve, reject} */
  var rpcRid = 1;
  var readyCbs = [];

  function send(obj) {
    postMessage(obj);
  }

  var ui = {
    /* ---- 对象生命周期 ---- */
    create: function (type, opts) {
      opts = opts || {};
      send({
        op: 'create',
        id: opts.id,
        type: type,
        parent: opts.parent || 'root'
      });
      if (opts.text != null) ui.set(opts.id, { text: opts.text });
      if (opts.x != null || opts.y != null)
        ui.set(opts.id, { x: opts.x | 0, y: opts.y | 0 });
      /* w/h 只发送给定项——|0 会把缺省的一侧变成 0（曾致 label h=0 不可见） */
      var size = {};
      if (opts.w != null) size.w = opts.w;
      if (opts.h != null) size.h = opts.h;
      if (size.w != null || size.h != null) ui.set(opts.id, size);
      if (opts.font) ui.set(opts.id, { font: opts.font });
      if (opts.center) ui.set(opts.id, { center: true });
      return opts.id;
    },
    set: function (id, props) {
      var msg = { op: 'set', id: id };
      for (var k in props) msg[k] = props[k];
      send(msg);
    },
    del: function (id) { send({ op: 'del', id: id }); },
    clear: function () { send({ op: 'clear' }); },
    focus: function (id) { send({ op: 'focus', id: id }); },
    refresh: function (full) { send({ op: 'refresh', full: !!full }); },

    /* ---- 事件订阅 ---- */
    on: function (id, event, fn) {
      handlers[id + ':' + event] = fn;
      send({ op: 'on', id: id, event: event });
    },
    off: function (id, event) {
      delete handlers[id + ':' + event];
    },
    onKey: function (fn) { keyHandlers.push(fn); },

    /* ---- 系统服务 RPC（uvrpc） ---- */
    rpc: function (method, params) {
      return new Promise(function (resolve, reject) {
        var rid = rpcRid++;
        rpcPending[rid] = { resolve: resolve, reject: reject };
        send({ op: 'rpc', rid: rid, method: method, params: params || {} });
      });
    },

    /* ---- 当前应用授权（宿主侧方法名边界）----
     *
     * 必须走独立的 op 而不是 ui.rpc：rpc 是**请求-应答**通道，需要宿主回
     * {evt:'rpc',rid}。而 op:app 是通知——宿主处理完不回任何东西。曾经把它
     * 写成 ui.rpc('app', …)，结果是请求发出去后宿主找不到这个 method、永不
     * 应答，promise 永远不 settle，.catch() 也永远不触发（它只处理
     * rejection，不处理「永不 settle」）。每次 launch/back 各泄漏一个
     * rpcPending 条目。50 MiB 的设备上不能这么攒。
     *
     * 顺带说明为什么通知类通道不该复用请求-应答通道：没有 rid、没有回执，
     * 宿主对它的沉默是**正确**行为，而调用方无法区分「处理完了」与
     * 「根本没听见」。
     */
    setApp: function (id, perms) {
      send({ op: 'app', id: id === undefined ? null : id, perms: perms || [] });
    },

    /* ---- 宿主事件分发（由 shell 的 __qzos_onmessage 调用） ---- */
    _dispatch: function (evt) {
      switch (evt.evt) {
      case 'click': {
        var cb = handlers[evt.id + ':click'];
        if (cb) cb(evt);
        break;
      }
      case 'value': {
        var cb2 = handlers[evt.id + ':value'];
        if (cb2) cb2(evt);
        break;
      }
      case 'key':
        keyHandlers.forEach(function (fn) { fn(evt.key); });
        break;
      case 'rpc': {
        var p = rpcPending[evt.rid];
        if (p) {
          delete rpcPending[evt.rid];
          if (evt.ok) {
            var res = evt.result;
            try { res = JSON.parse(evt.result); } catch (e) { /* keep raw */ }
            p.resolve(res);
          } else {
            p.reject(new Error(evt.result || 'rpc failed'));
          }
        }
        break;
      }
      case 'ready': {
        var cbs = readyCbs;
        readyCbs = [];
        cbs.forEach(function (fn) { fn(); });
        break;
      }
      case 'error':
        console.error('[host]', evt.msg);
        break;
      }
    },
    onReady: function (fn) { readyCbs.push(fn); }
  };

  global.ui = ui;
})(globalThis);
