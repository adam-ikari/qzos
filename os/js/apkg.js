/* apkg.js — qzos 应用包：manifest 校验、信任判定、能力计算
 *
 * 纯逻辑，**不依赖任何 qzjs 全局**（除了可选注入的 statMode）。这样它能被
 * 脱离宿主构建地单测——这是 os/docs/js-first.md 里「纯 JS 服务能脱离宿主单测」
 * 那条红利的直接兑现，也是本仓既有传统（test-display.sh 133 断言、
 * test-keymap.sh 82 断言都是毫秒级纯逻辑层）。
 *
 * 契约见 os/docs/app-package.md；授权执行点见 os/docs/sandbox.js。
 *
 * 注意加载形态：qzjs 的 eval 通道按 classic script 求值（不支持顶层 await），
 * 所以只用同步表达式挂全局，不做 ESM。
 */
(function () {
  'use strict';

  /* 宿主 UI 桥主版本。本包自己用到的 op（app/create/set/on/focus/rpc…）必须
   * 落在这个版本之内；应用声明的 api 超出它就拒绝启动。 */
  var HOST_API = 1;

  /* manifest 格式版本。不认识就拒绝，不猜——猜错的后果是应用带着错位的
   * 契约在系统里跑，比不启动更难查。 */
  var SCHEMA = 1;

  /* 已知能力表。perms 里出现表外的能力 → 拒绝启动整个应用，而不是忽略该项：
   * 忽略会让应用带着残缺的授权在系统里跑，而作者不知情。 */
  var CAPS = ['info', 'storage', 'settings', 'net', 'power'];

  /* ---- 路径：normalize + 包含判定 ----
   *
   * 不依赖 node 的 path（qzjs 里没有）。两条容易写错的：
   *   1) 归一化时 '..' 弹空栈必须判失败，而不是「忽略」——那正是 entry: "../x"
   *      能逃出去的根；
   *   2) 前缀比较必须带分隔符，否则 '/a/bc' 会被判成在 '/a/b' 里
   *      （经典同前缀漏洞）。
   */

  function normalize(p) {
    if (typeof p !== 'string' || p === '') return null;
    var abs = p.charAt(0) === '/';
    var out = [];
    var parts = p.split('/');
    for (var i = 0; i < parts.length; i++) {
      var s = parts[i];
      if (s === '' || s === '.') continue;
      if (s === '..') {
        if (out.length === 0) return null;   /* 弹空栈 = 逃出根 */
        out.pop();
      } else {
        out.push(s);
      }
    }
    return (abs ? '/' : '') + out.join('/');
  }

  /* candidate 是否在 root 之内（含 root 自身）。 */
  function pathInside(root, candidate) {
    var r = normalize(root), c = normalize(candidate);
    if (r === null || c === null) return false;
    if (c === r) return true;
    var prefix = (r === '/') ? '/' : (r + '/');
    return c.indexOf(prefix) === 0;
  }

  function basename(p) {
    var n = normalize(p);
    if (n === null) return null;
    if (n === '/') return '';
    var i = n.lastIndexOf('/');
    return i < 0 ? n : n.slice(i + 1);
  }

  function isPlainObject(v) {
    return v !== null && typeof v === 'object' && !Array.isArray(v);
  }

  /* ---- manifest 校验 ----
   *
   * 返回 {ok, errors:[{code, detail}]}。错误码是稳定契约，测试按码断言，
   * 不匹配错误文案——文案会改，码不会。
   */
  function validate(manifest, dirname, hostApi) {
    var errors = [];
    var api = (hostApi === undefined || hostApi === null) ? HOST_API : hostApi;

    function bad(code, detail) { errors.push({ code: code, detail: detail }); }

    if (!isPlainObject(manifest)) {
      bad('not-an-object', typeof manifest);
      return { ok: false, errors: errors };
    }
    if (manifest.schema !== SCHEMA) bad('schema', String(manifest.schema));

    /* id 必须等于目录名：perms 是按 id 审计的，id 漂移等于审计对象漂移，
     * 而两处不一致时没有任何权威来源可依。 */
    if (typeof manifest.id !== 'string' || manifest.id === '') {
      bad('id-missing', String(manifest.id));
    } else if (manifest.id !== basename(dirname)) {
      bad('id-mismatch', manifest.id + ' != ' + basename(dirname));
    }

    if (typeof manifest.name !== 'string' || manifest.name === '') bad('name', String(manifest.name));
    if (typeof manifest.version !== 'string' || manifest.version === '') bad('version', String(manifest.version));

    /* api：只比主版本。次版本差异不该拦启动。 */
    if (typeof manifest.api !== 'number' || !isFinite(manifest.api) ||
        Math.floor(manifest.api) !== manifest.api || manifest.api < 1) {
      bad('api-malformed', String(manifest.api));
    } else if (manifest.api > api) {
      bad('api-too-new', manifest.api + ' > ' + api);
    }

    /* entry：必须相对，且归一化后仍在应用目录内。 */
    if (typeof manifest.entry !== 'string' || manifest.entry === '') {
      bad('entry-missing', String(manifest.entry));
    } else if (manifest.entry.charAt(0) === '/') {
      bad('entry-absolute', manifest.entry);
    } else {
      var joined = normalize(dirname + '/' + manifest.entry);
      if (joined === null || !pathInside(dirname, joined)) {
        bad('entry-escape', manifest.entry);
      }
    }

    /* icon 同 entry 的规则（可选字段，错了也要拦）。 */
    if (manifest.icon !== undefined) {
      if (typeof manifest.icon !== 'string' || manifest.icon === '') {
        bad('icon-malformed', String(manifest.icon));
      } else if (manifest.icon.charAt(0) === '/') {
        bad('icon-absolute', manifest.icon);
      } else {
        var ij = normalize(dirname + '/' + manifest.icon);
        if (ij === null || !pathInside(dirname, ij)) bad('icon-escape', manifest.icon);
      }
    }

    /* perms：可缺省（= 空，default-deny），但给了就必须每项都在能力表里。 */
    if (manifest.perms !== undefined) {
      if (!Array.isArray(manifest.perms)) {
        bad('perms-malformed', typeof manifest.perms);
      } else {
        for (var i = 0; i < manifest.perms.length; i++) {
          var c = manifest.perms[i];
          if (typeof c !== 'string' || CAPS.indexOf(c) < 0) bad('perm-unknown', String(c));
        }
      }
    }

    return { ok: errors.length === 0, errors: errors };
  }

  /* ---- 信任与有效能力 ----
   *
   * 目录信任：应用目录到 apps-root 之间 group/other **不可写**才授予 perms。
   * 可写则**清空 perms 而非拒绝启动**——示例与随手写的应用不该因为权限位
   * 就被藏起来，而清空授权才是真正挡住误用的那一步。
   *
   * statMode 注入：JS 侧没有 stat（qzjs.fs 与 __native__ 都没有，实测），
   * 所以「目录模式」是宿主给的窄 C 原语。**拿不到就 fail-closed**：
   * 当作不可信 → 清空 perms。原因：静默当作可信等于把整个授权模型开后门，
   * 而且没人会发现——这正是 c1pkg 踩过的「mock 那半边形同虚设」。
   */
  function capsFor(manifest, trusted) {
    if (!trusted) return [];
    if (!isPlainObject(manifest) || !Array.isArray(manifest.perms)) return [];
    var out = [];
    for (var i = 0; i < manifest.perms.length; i++) {
      var c = manifest.perms[i];
      if (typeof c === 'string' && CAPS.indexOf(c) >= 0 && out.indexOf(c) < 0) out.push(c);
    }
    return out;
  }

  /* 目录是否可信：应用目录本身到 apps-root 之间每一段都不可 group/other 写。
   * statMode 缺失 → 不可信（fail-closed，见上）。
   *
   * 上界是 apps-root 而不是 /：这是有意的取舍。往上查到 / 的话，本机
   * /storage 是 0777，于是**任何**用户应用都永远拿不到 perms，授权模型在真机
   * 上等于全废。代价是「能整体替换 apps-root 的人」不受这条检查约束——那已经
   * 是「可以往设备里装任意应用」的威胁，属于另一个问题（见 app-package.md
   * 「信任规则」一节的残余风险）。 */
  function dirTrusted(dirname, appsRoot, statMode) {
    if (typeof statMode !== 'function') return false;
    var d = normalize(dirname), r = normalize(appsRoot);
    if (d === null || r === null) return false;
    var cur = d;
    while (true) {
      var m = statMode(cur);
      if (m === null || m === undefined) return false;   /* 查不到 = 不可信 */
      if ((m & 0o022) !== 0) return false;               /* group/other 可写 */
      if (cur === r) return true;
      var parent = cur.slice(0, cur.lastIndexOf('/')) || '/';
      if (parent === cur) return false;                   /* 到根还没碰到 r */
      cur = parent;
    }
  }

  /* ---- 能力 → 方法（方法名边界，C 侧 op_rpc 与 JS 侧共用同一规则） ----
   *
   * 能力 X 授予 sys.X 与 sys.X.*；不在 sys. 下的方法应用永远调不到。
   */
  function allows(caps, method) {
    if (!Array.isArray(caps) || typeof method !== 'string') return false;
    if (method.indexOf('sys.') !== 0) return false;
    var rest = method.slice(4);
    if (rest === '') return false;
    var cap = rest.split('.')[0];
    return caps.indexOf(cap) >= 0;
  }

  globalThis.QZOS_APKG = {
    HOST_API: HOST_API,
    SCHEMA: SCHEMA,
    CAPS: CAPS,
    normalize: normalize,
    pathInside: pathInside,
    basename: basename,
    validate: validate,
    capsFor: capsFor,
    dirTrusted: dirTrusted,
    allows: allows
  };
})();
