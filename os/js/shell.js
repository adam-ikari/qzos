/* shell.js — qzos 桌面 shell（由宿主 boot 脚本 eval 加载）
 *
 * 职责：
 *   1. 加载 ui.js 桥接层
 *   2. 用 qzjs.fs 自行发现 /storage 下的应用（目录含 app.json 者）
 *   3. 桌面 UI：应用列表（keypad group 焦点导航）
 *   4. 应用生命周期：launch / back 回桌面
 *   5. 系统键路由：back=返回/退出应用，home=回桌面
 *
 * e-ink 约束：无动画、无定时器（时钟等不进 shell；事件驱动 only）。
 */
(async function () {
  'use strict';

  var JS_DIR = globalThis.__QZ_JS_DIR || 'js';
  var APP_DIR = globalThis.__QZ_APP_DIR || '/storage';

  /* ---- 0. 加载桥接层 ---- */
  try {
    var uiSrc = await qzjs.fs.readFile(JS_DIR + '/ui.js');
    (0, eval)(uiSrc);
  } catch (err) {
    postMessage({ evt: 'error', msg: 'load ui.js failed: ' + err });
    return;
  }

  var ctx = null;   /* 当前运行的应用上下文；null = 在桌面 */

  /* ---- 1. 应用发现 ---- */

  async function scanDir(base) {
    var apps = [];
    var entries = [];
    try {
      entries = await qzjs.fs.readdir(base);
    } catch (err) {
      return apps; /* 目录不存在/无权限 */
    }
    for (var i = 0; i < entries.length; i++) {
      /* readdir 返回字符串数组（运行时实测）；兼容 {name,type} 对象形态 */
      var name = typeof entries[i] === 'string' ? entries[i] : entries[i].name;
      if (typeof entries[i] !== 'string' && entries[i].type !== 'dir') continue;
      var dir = base + '/' + name;
      var manifest = dir + '/app.json';
      if (!(await qzjs.fs.exists(manifest))) continue;
      try {
        var meta = JSON.parse(await qzjs.fs.readFile(manifest));
        if (meta && meta.name) {
          apps.push({
            id: name,
            name: meta.name,
            dir: dir,
            entry: dir + '/' + (meta.entry || 'app.js')
          });
        }
      } catch (e) { /* 坏 manifest：跳过 */ }
    }
    return apps;
  }

  /* 内置应用目录（js/apps）+ 用户应用目录（默认 /storage） */
  async function discoverApps() {
    var seen = {};
    var apps = [];
    var groups = [JS_DIR + '/apps', APP_DIR];
    for (var g = 0; g < groups.length; g++) {
      var found = await scanDir(groups[g]);
      for (var i = 0; i < found.length; i++) {
        if (seen[found[i].id]) continue;
        seen[found[i].id] = true;
        apps.push(found[i]);
      }
    }
    apps.sort(function (a, b) { return a.name < b.name ? -1 : 1; });
    return apps;
  }

  /* ---- 2. 桌面 UI ---- */

  function renderDesktop(apps) {
    ui.clear();
    ui.create('label', { id: 'dsl-title', text: 'qzos  ' + apps.length + ' apps',
                         x: 4, y: 4, w: 288, font: 'md' });

    var startY = 30, rowH = 26;
    apps.forEach(function (app, i) {
      var id = 'dsl-' + app.id;
      ui.create('btn', { id: id, parent: 'root', text: app.name,
                         x: 8, y: startY + i * rowH, w: 280, h: 22 });
      ui.on(id, 'click', function () { launch(app); });
    });
    if (apps.length) ui.focus('dsl-' + apps[0].id);
  }

  /* ---- 3. 应用生命周期 ---- */

  async function launch(app) {
    if (ctx) return;
    var api = {
      dir: app.dir,
      exit: function () { back(); }
    };
    try {
      var src = await qzjs.fs.readFile(app.entry);
      ui.clear();
      ctx = { app: app, api: api };
      /* 应用入口约定：定义 globalThis.App = { start(api) } 或直接跑顶层代码 */
      var mod = { exports: {} };
      var fn = new Function('api', 'module', 'exports',
                            src + '\n;return typeof App !== "undefined" ? App : module.exports;');
      var App = fn(api, mod, mod.exports);
      if (App && typeof App.start === 'function') await App.start(api);
    } catch (err) {
      postMessage({ evt: 'error', msg: 'launch ' + app.id + ': ' + err });
      ctx = null;
      renderDesktop(await listApps());
    }
  }

  async function back() {
    if (!ctx) return; /* 已在桌面 */
    var app = ctx.app;
    ctx = null;
    try {
      /* 让应用有机会收尾：再次 eval 不必要，约定应用在 window 留 onExit */
      if (typeof globalThis.App_onExit === 'function') globalThis.App_onExit();
    } catch (e) { /* ignore */ }
    delete globalThis.App;
    delete globalThis.App_onExit;
    renderDesktop(await listApps());
    void app;
  }

  async function listApps() {
    var apps = [];
    try { apps = await discoverApps(); } catch (e) { /* ignore */ }
    return apps;
  }

  /* ---- 4. 系统键路由 ---- */

  ui.onKey(function (key) {
    if (key === 'back') {
      back();
    } else if (key === 'home') {
      ctx = null;
      listApps().then(renderDesktop);
    } else if (key === 'volup' || key === 'voldown') {
      /* v1: 无背光服务，忽略 */
    }
  });

  /* ---- 5. 事件入口 ---- */

  globalThis.__qzos_onmessage = function (e) {
    var evt = e && e.data !== undefined ? e.data : e;
    if (evt && evt.evt) ui._dispatch(evt);
  };

  /* boot 流程：宿主 initial_script 已装好 dispatcher，随后 postMessage ready */
  var apps = await listApps();
  renderDesktop(apps);
  ui.refresh(true); /* 首帧全刷 */
  console.log('[shell] up, ' + apps.length + ' apps');

  /* 事件驱动为主，但主 RT 在「无事可做」时会 idle 自退（qzjs-rt
   * --qzjs-rt-server 形态），宿主随即收到 exited-unexpectedly。
   * 桌面壳在等用户按键，属于「有活」，用低频心跳把 loop 钉住。
   * 注意这不是动画：UI 不变，不产生任何重绘/提交。 */
  setInterval(function () { /* keep the shell alive, no redraw */ }, 5000);
})();
