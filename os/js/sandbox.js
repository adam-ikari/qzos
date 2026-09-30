/* sandbox.js — 授权执行点：在加载应用**之前**装好「面」
 *
 * 设计见 os/docs/app-package.md「注入点」一节。三条是被实测/读码定死的：
 *
 * 1) 必须同时遮 globalThis.__native__。它暴露 57 个原生，含 fsWrite/fsRemove/
 *    processSpawn/tcpConnect/contextSpawn/nativeEvalScript/selfPath……只换
 *    qzjs.fs 的话，应用一句 __native__.fsWrite(p,d) 就过去了——门遮了后门没遮。
 * 2) 面**永不还原**，只换指向哪个应用。否则 back() 还原真身这个动作本身开洞：
 *    应用先前排的 setTimeout 回调在还原之后触发，就拿到真的 qzjs.fs。
 * 3) 真身只进闭包。JS 枚举不到闭包变量，这是遮蔽成立的唯一原因。
 *
 * default-deny：原生面用**白名单**（表外一律不可见），不是黑名单。
 * 黑名单漏一项就是敞开的口子，而漏项不会有人发现。
 */
(function () {
  'use strict';

  /* 暴露给应用的原生白名单。刻意小：只放无害的纯函数/计时器/日志/随机。
   * 凡是碰文件系统、进程、TCP、context、worker、http、selfPath 的一律不给。 */
  var NATIVE_ALLOW = ['timeNow', 'hrtime', 'log', 'timerStart', 'timerStop', 'randomBytes'];

  /* qzjs.fs 上按应用目录做包含判定的成员。 */
  var FS_MEMBERS = ['readFile', 'readFileBinary', 'writeFile', 'exists', 'readdir', 'unlink'];

  function denied(path) {
    /* 抛错而不是静默返回 undefined：应用需要知道「被授权拒绝」和「文件不存在」
     * 是两件事，混起来会让上层写出错误的降级路径。 */
    var e = new Error('qzos: permission denied: ' + path);
    e.name = 'QZPermissionError';
    e.qzosDenied = true;
    e.path = path;
    throw e;
  }

  function install() {
    var apkg = globalThis.QZOS_APKG;
    if (!apkg) throw new Error('qzos: QZOS_APKG 未加载（apkg.js 必须在 sandbox.js 之前）');

    var realNative = globalThis.__native__;
    var realFs = globalThis.qzjs ? globalThis.qzjs.fs : null;

    /* 当前应用上下文。null = 桌面/系统自身（可信，不受限）。
     * 应用活动期间指向它的目录与能力集。 */
    var current = { dir: null, perms: [] };

    function resolveIn(dir, p) {
      /* 绝对路径不能拼到 app.dir 后面——那样 '/etc/passwd' 会变成
       * '<dir>//etc/passwd' 归一化成 '<dir>/etc/passwd'，反而判成包内。
       * 这是自己写测试时抓到的：目录内逃逸都挡住了，绝对路径却漏了。 */
      if (typeof p !== 'string' || p === '') return null;
      var abs = p.charAt(0) === '/';
      return apkg.normalize(abs ? p : (dir + '/' + p));
    }

    function pathOk(p) {
      /* 无活动应用 → 系统自己，全放。 */
      if (!current.dir) return true;
      var r = resolveIn(current.dir, p);
      if (r === null) return false;
      return apkg.pathInside(current.dir, r);
    }

    function check(p) {
      if (!pathOk(p)) denied(p);
    }

    /* ---- fs 面 ---- */
    var fsFacade = {};
    FS_MEMBERS.forEach(function (name) {
      fsFacade[name] = function (p) {
        check(p);
        return realFs[name].apply(realFs, arguments);
      };
    });
    /* 同步别名本来在 qzjs 里就抛异常；保留同语义，别给它开后门。 */
    fsFacade.readFileSync = function () {
      throw new Error('Synchronous fs operations not supported in qzjs');
    };

    /* ---- native 面（白名单） ---- */
    var nativeFacade = {};
    NATIVE_ALLOW.forEach(function (name) {
      if (realNative && typeof realNative[name] === 'function') {
        nativeFacade[name] = realNative[name].bind(realNative);
      }
    });

    /* 装面，且此后不再换回真身。 */
    if (realNative) globalThis.__native__ = nativeFacade;
    if (realFs) globalThis.qzjs.fs = fsFacade;

    return {
      /* shell 在 launch 前调；perms 来自 manifest 经校验+信任计算后的结果。 */
      setApp: function (dir, perms) {
        current.dir = dir || null;
        current.perms = Array.isArray(perms) ? perms.slice() : [];
      },
      clearApp: function () { current.dir = null; current.perms = []; },
      current: current,
      /* 供测试与调试：判断某路径当前是否可访问，不抛错。 */
      wouldAllow: function (p) { return pathOk(p); }
    };
  }

  globalThis.QZOS_SANDBOX = {
    NATIVE_ALLOW: NATIVE_ALLOW,
    FS_MEMBERS: FS_MEMBERS,
    install: install
  };
})();
