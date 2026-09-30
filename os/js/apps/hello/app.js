/* hello — qzos 示例应用
 * 演示：创建按钮、click 事件、系统 RPC 调用、api.exit 返回桌面。
 */
var App = {
  start: async function (api) {
    ui.create('label', { id: 'hi', text: 'Hello, qzos!', x: 8, y: 12, w: 280, font: 'md' });
    ui.create('label', { id: 'hi-sub', text: 'fusion pixel font / e-ink', x: 8, y: 40, w: 280, font: 'sm' });

    ui.create('btn', { id: 'hi-rpc', text: 'sys.info (rpc)', x: 8, y: 74, w: 136, h: 24 });
    ui.create('btn', { id: 'hi-back', text: 'back', x: 152, y: 74, w: 136, h: 24 });

    ui.create('label', { id: 'hi-out', text: '', x: 8, y: 110, w: 280, font: 'sm' });

    ui.on('hi-rpc', 'click', async function () {
      try {
        var r = await ui.rpc('sys.info', {});
        ui.set('hi-out', { text: 'svc=' + (r.service || '?') + ' pid=' + (r.pid || '?') });
      } catch (e) {
        ui.set('hi-out', { text: 'rpc error: ' + e.message });
      }
    });
    ui.on('hi-back', 'click', function () { api.exit(); });

    ui.focus('hi-rpc');
  }
};
