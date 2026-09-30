/* test_apkg.js — apkg.js / sandbox.js 单测（跑在真实 qzjs 上）
 *
 * 刻意用真实 qzjs 而不是 node：本测试的核心断言是「globalThis.__native__
 * 上那 57 个原生真的被遮住了」，而 __native__ 是 qzjs 宿主注入的——在 node 上
 * 跑这个测试等于什么都没测。
 *
 * 加载真实模块（qzjs.fs.readFile + eval），不走被测代码里的任何捷径。
 * 工作根目录由宿主注入 globalThis.__QZOS_TEST_ROOT（qzjs CLI 没有 argv；
 * 可用的是 globalThis.arguments，值由 scripts/test-apkg.sh 设好）。
 *
 * 布局：纯逻辑断言（同步）先跑完，再跑遮蔽断言（异步）。纯逻辑部分不碰任何
 * 被遮的面，所以放在装面之前跑，避免自己把自己的脚砍了。
 */
(function () {
  'use strict';

  var ROOT = globalThis.__QZOS_TEST_ROOT || '/tmp';
  var APP = ROOT + '/app';
  var SIB = ROOT + '/sibling';
  var checks = 0, failed = 0, failures = [];

  function ok(cond, name, extra) {
    checks++;
    if (!cond) { failed++; failures.push(name + (extra ? ' :: ' + extra : '')); }
  }
  function eq(got, want, name) {
    ok(got === want, name, 'got=' + JSON.stringify(got) + ' want=' + JSON.stringify(want));
  }
  function hasCode(res, code, name) {
    var codes = res.errors.map(function (e) { return e.code; });
    ok(codes.indexOf(code) >= 0, name, 'codes=' + codes.join(','));
  }
  function done() {
    if (failed === 0) {
      console.log('OK: ' + checks + ' checks, 0 failed');
    } else {
      console.log('FAILED: ' + failed + ' of ' + checks);
      for (var i = 0; i < failures.length; i++) console.log('  - ' + failures[i]);
    }
  }
  function bail(e) {
    failed++;
    failures.push('意外异常: ' + ((e && e.stack) || e));
    done();
  }

  /* ================= 1. 纯逻辑：路径 ================= */

  var A = globalThis.QZOS_APKG;

  eq(A.normalize('/a/b/../c'), '/a/c', 'normalize: 弹栈');
  eq(A.normalize('/a/./b//c'), '/a/b/c', 'normalize: 去空与点');
  eq(A.normalize('../x'), null, 'normalize: 弹空栈判失败（否则 ../x 能逃出去）');
  eq(A.normalize('/a/../../x'), null, 'normalize: 越过根判失败');

  ok(A.pathInside('/a/b', '/a/b/c.js'), 'pathInside: 子路径在内');
  ok(A.pathInside('/a/b', '/a/b'), 'pathInside: 自身在内');
  ok(!A.pathInside('/a/b', '/a/bc'), 'pathInside: 同前缀兄弟不算在内（经典漏洞）');
  ok(!A.pathInside('/a/b', '/a'), 'pathInside: 上级不在内');
  ok(!A.pathInside('/a/b', '/x'), 'pathInside: 完全无关');
  ok(!A.pathInside('/a/b', '/a/b/../../etc'), 'pathInside: 逃逸被拒');
  ok(A.pathInside('/', '/anything'), 'pathInside: 根包含一切');

  eq(A.basename('/a/b/notepad'), 'notepad', 'basename: 取目录名');
  eq(A.basename('/'), '', 'basename: 根为空');

  /* ================= 2. 纯逻辑：manifest 校验 ================= */

  var GOOD = { schema: 1, id: 'notepad', name: 'Notepad', version: '1.0.0', api: 1, entry: 'app.js' };
  var D = '/storage/apps/notepad';

  ok(A.validate(GOOD, D).ok, 'validate: 正例通过');
  ok(A.validate(GOOD, D, 2).ok, 'validate: 宿主 api 更高也过');
  hasCode(A.validate({ schema: 2, id: 'notepad', name: 'N', version: '1', api: 1, entry: 'app.js' }, D), 'schema', 'validate: 未知 schema 拒绝');
  hasCode(A.validate({ schema: 1, id: 'other', name: 'N', version: '1', api: 1, entry: 'app.js' }, D), 'id-mismatch', 'validate: id 必须等于目录名');
  hasCode(A.validate({ schema: 1, id: 'notepad', name: 'N', version: '1', api: 99, entry: 'app.js' }, D), 'api-too-new', 'validate: api 超宿主拒绝');
  hasCode(A.validate({ schema: 1, id: 'notepad', name: 'N', version: '1', api: 1, entry: '../evil/app.js' }, D), 'entry-escape', 'validate: entry 逃逸被拒');
  hasCode(A.validate({ schema: 1, id: 'notepad', name: 'N', version: '1', api: 1, entry: '/etc/passwd' }, D), 'entry-absolute', 'validate: entry 绝对路径被拒');
  hasCode(A.validate({ schema: 1, id: 'notepad', name: 'N', version: '1', api: 1, entry: 'app.js', perms: ['sudo'] }, D), 'perm-unknown', 'validate: 表外能力拒绝启动（不是忽略）');
  hasCode(A.validate({ schema: 1, id: 'notepad', name: '', version: '1', api: 1, entry: 'app.js' }, D), 'name', 'validate: 缺 name 拒绝');
  hasCode(A.validate({ schema: 1, id: 'notepad', name: 'N', version: '1', api: 1, entry: 'app.js', icon: '../../x.pbm' }, D), 'icon-escape', 'validate: icon 逃逸被拒');
  hasCode(A.validate({ schema: 1, id: 'notepad', name: 'N', version: '1', api: 1, entry: 'app.js', perms: 'info' }, D), 'perms-malformed', 'validate: perms 非数组拒绝');
  ok(!A.validate(null, D).ok, 'validate: 非对象拒绝');
  ok(!A.validate({ schema: 1, id: 'notepad', name: 'N', version: '1', api: 1.5, entry: 'app.js' }, D).ok, 'validate: api 非整数拒绝');

  /* ================= 3. 纯逻辑：能力 → 方法 ================= */

  ok(A.allows(['storage'], 'sys.storage'), 'allows: 能力授予同名方法');
  ok(A.allows(['storage'], 'sys.storage.list'), 'allows: 能力授予前缀方法');
  ok(!A.allows(['storage'], 'sys.settings'), 'allows: 别的能力不被顺带放行（只测放行会漏掉前缀写错）');
  ok(!A.allows([], 'sys.info'), 'allows: default-deny');
  ok(!A.allows(['info'], 'sys.power.shutdown'), 'allows: 关机不被 info 顺带放行');
  ok(!A.allows(['info'], 'fsWrite'), 'allows: 非 sys.* 方法永远调不到');
  ok(!A.allows(['info'], 'sys.'), 'allows: 空前缀拒绝');

  /* ================= 4. 纯逻辑：能力与信任 ================= */

  eq(A.capsFor({ perms: ['info', 'storage'] }, true).join(','), 'info,storage', 'capsFor: 可信则给');
  eq(A.capsFor({ perms: ['info'] }, false).join(','), '', 'capsFor: 不可信则清空（不是拒绝启动）');
  eq(A.capsFor({}, true).join(','), '', 'capsFor: 缺 perms = 无授权');
  eq(A.capsFor({ perms: 'info' }, true).join(','), '', 'capsFor: perms 非数组 = 无授权');
  eq(A.capsFor({ perms: ['info', 'info'] }, true).join(','), 'info', 'capsFor: 去重');

  /* statMode 是宿主注入的窄 C 原语：JS 侧没有 stat（qzjs.fs 与 __native__ 都没有）。 */
  var MODES = { '/storage/apps': 0o755, '/storage/apps/notepad': 0o755, '/storage': 0o777 };
  function statMode(p) { return (p in MODES) ? MODES[p] : null; }

  ok(A.dirTrusted('/storage/apps/notepad', '/storage/apps', statMode), 'dirTrusted: 全链不可写则可信');
  MODES['/storage/apps/notepad'] = 0o777;
  ok(!A.dirTrusted('/storage/apps/notepad', '/storage/apps', statMode), 'dirTrusted: 应用目录可写则不可信');
  MODES['/storage/apps/notepad'] = 0o755;
  MODES['/storage/apps'] = 0o775;
  ok(!A.dirTrusted('/storage/apps/notepad', '/storage/apps', statMode), 'dirTrusted: 祖先目录可写则不可信（c1pkg 同类坑）');
  ok(!A.dirTrusted('/storage/apps/notepad', '/storage/apps', undefined), 'dirTrusted: 缺 statMode 则 fail-closed');
  ok(!A.dirTrusted('/storage/apps/nope', '/storage/apps', statMode), 'dirTrusted: 查不到模式则 fail-closed');
  /* 上界是 apps-root（有意取舍）：本机 /storage 是 0777，若往上查到 / 则任何
   * 用户应用都永远拿不到 perms，模型在真机上全废。钉住这个取舍，免得日后
   * 有人「顺手修正」成查全链。 */
  MODES['/storage/apps'] = 0o755;
  ok(A.dirTrusted('/storage/apps/notepad', '/storage/apps', statMode), 'dirTrusted: apps-root 之上不参与判定（有意的上界）');
  ok(!A.dirTrusted('/storage/apps/notepad', '/storage', statMode), 'dirTrusted: apps-root 本身可写则不可信');

  /* ================= 5. 遮蔽：必须在真实 qzjs 上跑 ================= */

  var n0 = globalThis.__native__;
  ok(n0 && typeof n0 === 'object', '前提: __native__ 存在');
  var nativeCount = n0 ? Object.keys(n0).length : 0;
  ok(nativeCount > 20, '前提: __native__ 暴露大量原生（实测 ' + nativeCount + ' 个）');

  var box = globalThis.QZOS_SANDBOX.install();

  /* 后门。只测 qzjs.fs 被遮是不够的——门面遮了后门没遮，全绿。 */
  eq(typeof globalThis.__native__.fsWrite, 'undefined', '遮蔽: __native__.fsWrite 不可见');
  eq(typeof globalThis.__native__.fsRemove, 'undefined', '遮蔽: __native__.fsRemove 不可见');
  eq(typeof globalThis.__native__.fsWriteSync, 'undefined', '遮蔽: __native__.fsWriteSync 不可见');
  eq(typeof globalThis.__native__.fsReadBinary, 'undefined', '遮蔽: __native__.fsReadBinary 不可见');
  eq(typeof globalThis.__native__.processSpawn, 'undefined', '遮蔽: __native__.processSpawn 不可见');
  eq(typeof globalThis.__native__.processTerminate, 'undefined', '遮蔽: processTerminate 不可见');
  eq(typeof globalThis.__native__.contextSpawn, 'undefined', '遮蔽: contextSpawn 不可见');
  eq(typeof globalThis.__native__.tcpConnect, 'undefined', '遮蔽: tcpConnect 不可见');
  eq(typeof globalThis.__native__.nativeEvalScript, 'undefined', '遮蔽: nativeEvalScript 不可见');
  eq(typeof globalThis.__native__.selfPath, 'undefined', '遮蔽: selfPath 不可见');
  eq(typeof globalThis.__native__.spawnWorker, 'undefined', '遮蔽: spawnWorker 不可见');
  eq(typeof globalThis.__native__.httpRequest, 'undefined', '遮蔽: httpRequest 不可见');
  eq(typeof globalThis.__native__.storageSet, 'undefined', '遮蔽: storageSet 不可见（否则能改系统级 kv）');
  /* 白名单里的必须还在，否则系统自己也没法记日志 */
  eq(typeof globalThis.__native__.log, 'function', '遮蔽: 白名单 log 仍可用');
  eq(typeof globalThis.__native__.randomBytes, 'function', '遮蔽: 白名单 randomBytes 仍可用');
  eq(typeof globalThis.__native__.timeNow, 'function', '遮蔽: 白名单 timeNow 仍可用');

  /* 桌面态：无活动应用时系统自己不受限 */
  ok(box.wouldAllow('/etc/passwd'), '桌面态: 路径不受限（系统自己可信）');

  box.setApp(APP, []);
  ok(box.wouldAllow('app.js'), '应用态: 包内相对路径可写');
  ok(box.wouldAllow('lib/a.js'), '应用态: 包内子目录可写');
  ok(box.wouldAllow(APP + '/app.js'), '应用态: 包内绝对路径可写');
  ok(!box.wouldAllow('../sibling/secret.txt'), '应用态: 逃到兄弟目录被拒');
  ok(!box.wouldAllow('/etc/passwd'), '应用态: 绝对路径包外被拒');
  ok(!box.wouldAllow('../../etc/passwd'), '应用态: 相对逃逸被拒');
  ok(!box.wouldAllow(APP + '/../sibling/x'), '应用态: 绝对路径恰好逃到兄弟目录也被拒');
  ok(!box.wouldAllow(''), '应用态: 空路径被拒');
  ok(!box.wouldAllow(null), '应用态: 非字符串被拒');

  /* 越权必须**同步**抛，而不是返回将来会 reject 的 Promise。契约级：应用写成
   * qzjs.fs.writeFile(p,d).catch(...) 时，同步抛出绕过了那个 catch，错误变成
   * 未处理 rejection 而被静默吞掉。 */
  var r1 = false, n1 = '';
  try { qzjs.fs.readFile(SIB + '/secret.txt'); } catch (e) { r1 = !!e.qzosDenied; n1 = e.name; }
  ok(r1, '应用态: 越权 readFile 调用即抛（' + n1 + '）');

  var r2 = false;
  try { qzjs.fs.writeFile(SIB + '/evil.txt', 'pwned'); } catch (e) { r2 = !!e.qzosDenied; }
  ok(r2, '应用态: 越权 writeFile 调用即抛（同步，不是 rejected Promise）');

  var r3 = false;
  try { qzjs.fs.unlink(SIB + '/secret.txt'); } catch (e) { r3 = !!e.qzosDenied; }
  ok(r3, '应用态: 越权 unlink 被拒（否则应用能删掉别人的文件）');

  var r4 = false;
  try { qzjs.fs.readdir(SIB); } catch (e) { r4 = !!e.qzosDenied; }
  ok(r4, '应用态: 越权 readdir 被拒（否则能枚举别人的文件）');

  /* ================= 6. 落盘效果断言 ================= */

  var inside = APP + '/.inside-probe';
  var siblingBefore = null;

  qzjs.fs.writeFile(inside, 'x')
    .then(function () { return qzjs.fs.readFile(inside); })
    .then(function (content) {
      eq(content, 'x', '落盘: 包内真能读写（效果断言，不是「没抛异常」）');
      return qzjs.fs.unlink(inside);
    })
    .then(function () {
      /* back：清空应用上下文，回到桌面态（系统自己不受限）。
       * 顺序很重要：上面那些「桌面态: 路径不受限」是同步断言，必须在
       * setApp 之前就已经成立，所以这里才清。 */
      box.clearApp();
      /* 桌面态读兄弟诱饵，作为「这条越权路径确实可达」的基线 */
      return qzjs.fs.readFile(SIB + '/secret.txt');
    })
    .then(function (content) {
      siblingBefore = content;
      eq(siblingBefore, 'TOP-SECRET\n', '桌面态: 能读到兄弟诱饵（基线成立）');

      /* 关键判据：模拟「应用还没退干净」——在飞的回调重新进入应用上下文。
       * 若实现是 back 时把真身装回去，这里就会拿到真 fs 并写到 SIB。
       * 正确实现下 SIB 在包外，必须同步抛。 */
      box.setApp(APP, []);
      var escaped = false, why = 'no-throw';
      try {
        qzjs.fs.writeFile(SIB + '/backdoor.txt', 'pwned');
        escaped = true;
      } catch (e) {
        why = e.name;
      }
      box.clearApp();
      ok(!escaped, '面常驻: back 之后在飞的回调拿不到真身（' + why + '）');
    })
    .then(function () {
      /* 效果断言：越权写真的没落地 */
      return qzjs.fs.readFile(SIB + '/secret.txt');
    })
    .then(function (after) {
      eq(after, siblingBefore, '面常驻: 越权写未落地（诱饵内容不变）');
      return qzjs.fs.exists(SIB + '/backdoor.txt');
    })
    .then(function (ex) {
      ok(ex === false, '面常驻: backdoor.txt 确实未被创建');
      done();
    })
    .catch(bail);
})();
