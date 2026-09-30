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

  /* ---- 0. 加载桥接层与应用模型，并装好授权面 ----
   *
   * 顺序是有讲究的，不能调换：
   *   ui.js      —— 桥接层本身
   *   apkg.js    —— manifest 校验 / 信任 / 能力（纯逻辑）
   *   sandbox.js —— 授权面（要读走真身，所以必须在任何应用加载之前装）
   * 装完之后整个进程里就再没有「未遮蔽的 fs / __native__」这个东西了，
   * 包括本文件自己——这正是设计意图：系统自己也走面，桌面态（无活动应用）
   * 才不受限。
   */
  try {
    (0, eval)(await qzjs.fs.readFile(JS_DIR + '/ui.js'));
    (0, eval)(await qzjs.fs.readFile(JS_DIR + '/apkg.js'));
    (0, eval)(await qzjs.fs.readFile(JS_DIR + '/sandbox.js'));
  } catch (err) {
    postMessage({ evt: 'error', msg: 'load shell modules failed: ' + err });
    return;
  }

  var APKG = globalThis.QZOS_APKG;
  var sandbox = globalThis.QZOS_SANDBOX.install();

  var ctx = null;        /* 当前运行的应用上下文；null = 在桌面或坏包详情页 */
  var s_on_detail = false; /* 坏包详情页是否正在显示（back 的判据，见 back()） */
  var requireCache = {}; /* api.require 的模块缓存，每次 back 清空 */

  /* ---- 1. 应用发现 ---- */

  async function scanDir(base, builtin) {
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
      var meta = null;
      try {
        meta = JSON.parse(await qzjs.fs.readFile(manifest));
      } catch (e) {
        apps.push(broken(name, dir, 'manifest 不是合法 JSON'));
        continue;
      }
      /* 五条校验一次做完（os/docs/app-package.md）。任一条不过就**不装**——
       * 注意是「拒绝启动这个应用」而不是「降级启动」：带着残缺契约跑起来
       * 比不跑更难查。
       *
       * 目录信任分两路，**不能一刀切 fail-closed**：
       *
       *  - 用户目录（APP_DIR，默认 /storage）：statMode 原语还没实现（JS 侧
       *    没有 stat，qzjs.fs 与 __native__ 都没有，实测），所以判定为不可信
       *    → perms 清空。这是 fail-closed：静默当作可信等于给整个授权模型
       *    开后门，且没人会发现。
       *  - **内置目录（JS_DIR/apps，随仓发布、只读 rootfs 上不可写）不在这条
       *    规则之内**：它是系统的一部分，可信性来自「随仓发布」而不是「目录
       *    权限位」。早先一刀切 fail-closed 之后，内置的 hello 拿不到
       *    perms:["info"]，它的 sys.info 按钮静默失效——而 11 项键盘端到端
       *    全绿，因为那条断言只看「画面变了没」，没看画面上是不是错误信息。
       *    这就是「回执/表象层面的断言替代效果层面断言」又中一次。
       */
      var v = APKG.validate(meta, dir);
      if (!v.ok) {
        apps.push(broken(name, dir, v.errors.map(function (e) { return e.code; }).join(',')));
        continue;
      }
      var trusted = builtin ? true : APKG.dirTrusted(dir, base, null);
      apps.push({
        id: meta.id,
        name: meta.name,
        dir: dir,
        entry: APKG.normalize(dir + '/' + meta.entry),
        perms: APKG.capsFor(meta, trusted),
        trusted: trusted,
        broken: null
      });
    }
    return apps;
  }

  function broken(id, dir, why) {
    return { id: id, name: id, dir: dir, entry: null, perms: [], trusted: false, broken: why };
  }

  /* 内置应用目录（js/apps）+ 用户应用目录（默认 /storage） */
  async function discoverApps() {
    var seen = {};
    var apps = [];
    var groups = [JS_DIR + '/apps', APP_DIR];
    for (var g = 0; g < groups.length; g++) {
      var found = await scanDir(groups[g], g === 0);
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
    s_on_detail = false;
    var ok_ = apps.filter(function (a) { return !a.broken; });
    var bad = apps.filter(function (a) { return a.broken; });
    ui.create('label', { id: 'dsl-title',
                         text: 'qzos  ' + ok_.length + ' apps' + (bad.length ? '  ' + bad.length + ' bad' : ''),
                         x: 4, y: 4, w: 288, font: 'md' });

    var startY = 30, rowH = 26;
    ok_.forEach(function (app, i) {
      var id = 'dsl-' + app.id;
      ui.create('btn', { id: id, parent: 'root', text: app.name,
                         x: 8, y: startY + i * rowH, w: 280, h: 22 });
      ui.on(id, 'click', function () { launch(app); });
    });
    /* 坏包也**列出来**，只是标出来——静默隐藏会让人以为「装上了」，
     * 而真机上没人会去读日志。e-ink 上不做颜色区分，用前缀字符标记。 */
    bad.forEach(function (app, i) {
      var id = 'bad-' + app.id;
      ui.create('btn', { id: id, parent: 'root', text: '! ' + app.id,
                         x: 8, y: startY + (ok_.length + i) * rowH, w: 280, h: 22 });
      ui.on(id, 'click', function () { showBroken(app); });
    });
    if (ok_.length) ui.focus('dsl-' + ok_[0].id);
  }

  function showBroken(app) {
    ui.clear();
    s_on_detail = true;
    ui.create('label', { id: 'bad-t', text: app.id, x: 4, y: 4, w: 288, font: 'md' });
    ui.create('label', { id: 'bad-why', text: String(app.broken).slice(0, 120),
                         x: 4, y: 34, w: 288, font: 'sm' });
    var back2 = 'bad-back';
    ui.create('btn', { id: back2, text: 'back', x: 8, y: 100, w: 280, h: 22 });
    ui.on(back2, 'click', function () { back(); });
    ui.focus(back2);
  }

  /* ---- 3. 应用生命周期 ---- */

  async function launch(app) {
    if (ctx) return;
    if (app.broken) { showBroken(app); return; }

    /* 装当前应用上下文（授权面 + 宿主侧方法名边界）。必须在**读 entry 之前**：
     * 读文件本身就走面，面必须先到位，否则这一步是未授权的。 */
    sandbox.setApp(app.dir, app.perms);
    /* 通知类，走独立的 op（不是 ui.rpc——那条要回执，用在这里会挂死）。
     * 必须在**读 entry 之前**发：读文件走面，面要先到位。 */
    ui.setApp(app.id, app.perms);

    /* 每次 launch 用**新数组**，不重置模块级那个：back() 可能与 launch 交错，
     * 重置模块级数组会把上一个应用还没跑的收尾回调清掉。 */
    var myHooks = [];
    var api = {
      dir: app.dir,
      id: app.id,
      /* 已授予能力的只读副本：UI 据此隐藏做不到的按钮，而不是点了才报错。
       * 应用不该靠「试一下看能不能」来决定给用户显示什么操作。 */
      perms: Object.freeze(app.perms.slice()),
      can: function (cap) { return api.perms.indexOf(cap) >= 0; },
      exit: function () { back(); },
      onExit: function (fn) { if (typeof fn === 'function') myHooks.push(fn); },
      /* 同包内模块加载。归一化后必须仍在应用目录内（apkg.pathInside 判），
       * 越界的直接拒——不是交给 fs 去报错，因为那时已经读到了。 */
      require: async function (rel) {
        var p = APKG.normalize(app.dir + '/' + rel);
        if (!APKG.pathInside(app.dir, p)) throw new Error('qzos: require 越界: ' + rel);
        if (requireCache[p]) return requireCache[p];
        var src = await qzjs.fs.readFile(p);
        var m = { exports: {} };
        /* 形态与 launch 入口一致：模块用 module.exports 导出。
         * 先登记再求值，模块里的循环 require 才不会无限递归。 */
        requireCache[p] = m;
        var wrapper = new Function('module', 'exports', 'require',
                                   src + '\n;return module.exports;');
        wrapper(m, m.exports, api.require);
        return m.exports;
      }
    };

    try {
      var src = await qzjs.fs.readFile(app.entry);
      ui.clear();
      ctx = { app: app, api: api, exitHooks: myHooks };
      var mod = { exports: {} };
      var fn = new Function('api', 'module', 'exports', 'require',
                            src + '\n;return typeof App !== "undefined" ? App : module.exports;');
      var App = fn(api, mod, mod.exports, api.require);
      if (App && typeof App.start === 'function') await App.start(api);
      if (App && typeof App.onExit === 'function') myHooks.push(App.onExit);
    } catch (err) {
      postMessage({ evt: 'error', msg: 'launch ' + app.id + ': ' + err });
      /* 启动失败也必须收权并清面，否则桌面态下残留的应用授权会一直生效 */
      sandbox.clearApp();
      ui.setApp(null, []);
      ctx = null;
      renderDesktop(await listApps());
    }
  }

  async function back() {
    /* ctx 为空**不等于**「无事可做」。坏包详情页就是 ctx 为空的状态——那里
     * 仍然需要 back 能退回桌面。早先写成 `if (!ctx) return;`，于是详情页上的
     * back 按钮与系统 back 键都退不出去，用户被卡住只能重启。
     * 「已在桌面」的正确判据是画面上有没有桌面，而不是有没有应用上下文。 */
    if (!ctx) {
      /* 已在桌面就别重画：否则按 back 会引发一次全刷，白耗墨水屏寿命
       * （e-ink 硬约束：无事不刷）。详情页与桌面都能靠 title 区分。 */
      if (!s_on_detail) return;
      s_on_detail = false;
      renderDesktop(await listApps());
      return;
    }
    /* 顺序：先取钩子 → 再清 ctx/收权 → 最后跑钩子。应用代码在 onExit 里
     * 还可能碰 fs / rpc，此时它应该已经没有授权了；反过来做等于给
     * 「正在退出的应用」多留一段授权窗口。 */
    var hooks = ctx.exitHooks ? ctx.exitHooks.slice() : [];
    ctx = null;
    s_on_detail = false;
    sandbox.clearApp();
    ui.setApp(null, []);
    /* 清掉模块缓存：下次 launch 重新求值，否则上一个应用的模块状态会漏给下一个 */
    for (var k in requireCache) { delete requireCache[k]; }
    try {
      for (var i = 0; i < hooks.length; i++) {
        try { hooks[i](); } catch (e) { /* 单个收尾失败不挡其它 */ }
      }
      if (typeof globalThis.App_onExit === 'function') globalThis.App_onExit();
    } catch (e) { /* ignore */ }
    delete globalThis.App;
    delete globalThis.App_onExit;
    renderDesktop(await listApps());
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
      /* home 强制回桌面：即便有应用在跑也要收权 + 跑收尾钩子，走 back() 而不是
       * 只把 ctx 置空——否则会漏掉 onExit 与授权清理。 */
      back();
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

  /* 曾经这里有个 setInterval(…, 5000) 心跳，注释说「主 RT 会 idle 自退，
   * 心跳把它钉住」。实测 25 秒无按键：主 RT **没有**自退（qzjs_rt_host.c 的
   * 崩溃检测只在「已 ready 且非预期退出」时才推错误帧，idle 自退有独立的
   * idle_ack 通道，而 ISOLATED 的主RT 是常驻进程）。所以那个心跳的前提
   * 不成立，留着只是白耗电。
   *
   * 真要钉住 loop，正确的位置是宿主侧（有活干的判据在 C 侧），不是 JS 侧
   * 烧一个定时器。JS 侧保持纯事件驱动——这也符合 e-ink「无事不刷」。 */
})();
