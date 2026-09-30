/* notepad — qzos 示例应用：keypad 输入 + qzjs.fs 持久化
 * 方向键/Enter 走 LVGL textarea；back 键回桌面（shell 路由）。
 */
var App = {
  start: async function (api) {
    var path = api.dir + '/note.txt';

    ui.create('label', { id: 'np-title', text: 'notepad', x: 4, y: 4, w: 288, font: 'md' });
    ui.create('ta', { id: 'np-ta', x: 4, y: 28, w: 288, h: 90 });
    ui.create('btn', { id: 'np-save', text: 'save', x: 4, y: 124, w: 92, h: 22 });
    ui.create('btn', { id: 'np-load', text: 'load', x: 102, y: 124, w: 92, h: 22 });
    ui.create('btn', { id: 'np-back', text: 'back', x: 200, y: 124, w: 92, h: 22 });
    ui.create('label', { id: 'np-status', text: '', x: 4, y: 150, w: 288, font: 'sm' });

    try {
      if (await qzjs.fs.exists(path)) {
        var saved = await qzjs.fs.readFile(path);
        ui.set('np-ta', { text: saved });
        ui.set('np-status', { text: 'loaded ' + saved.length + ' bytes' });
      }
    } catch (e) {
      ui.set('np-status', { text: 'load failed' });
    }

    ui.on('np-save', 'click', async function () {
      try {
        await qzjs.fs.writeFile(path, App._text || '');
        ui.set('np-status', { text: 'saved' });
        ui.refresh(false);
      } catch (e) {
        ui.set('np-status', { text: 'save failed: ' + e });
      }
    });
    ui.on('np-load', 'click', async function () {
      try {
        var t = await qzjs.fs.readFile(path);
        ui.set('np-ta', { text: t });
        ui.set('np-status', { text: 'loaded' });
      } catch (e) {
        ui.set('np-status', { text: 'no note yet' });
      }
    });
    ui.on('np-back', 'click', function () { api.exit(); });
    ui.on('np-ta', 'value', function (e) { App._text = e.text; });

    ui.focus('np-ta');
  },
  _text: ''
};
