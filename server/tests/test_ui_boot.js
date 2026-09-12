// 直接测试页面实际交付的内联脚本，避免生成的 inline.js 与页面不同步。
const fs = require('fs');
const vm = require('vm');
const path = require('path');
const assert = require('node:assert/strict');

const page = fs.readFileSync(path.join(__dirname, '..', 'index.html'), 'utf8');
const script = [...page.matchAll(/<script>([\s\S]*?)<\/script>/g)].map(match => match[1]).join('\n');
assert.ok(script.trim(), '页面必须包含内联脚本');
const results = [];
function check(name, condition) {
  results.push(Boolean(condition));
  console.log((condition ? '  PASS ' : '  FAIL ') + name);
}
function response(data, status = 200) {
  return {ok: status >= 200 && status < 300, status, json: async () => data};
}
function makeDom() {
  const elements = new Map();
  for (const match of page.matchAll(/<[^>]+\bid="([^"]+)"[^>]*>/g)) {
    const listeners = new Map();
    let html = '';
    elements.set(match[1], {
      id: match[1], hidden: /\shidden(?:\s|>)/.test(match[0]),
      textContent: '', value: match[1] === 'statusFilter' ? 'all' : '',
      disabled: false, scrollLeft: 0, writes: 0, listeners,
      get innerHTML() { return html; },
      set innerHTML(value) { html = value; this.writes++; },
      focus() {},
      addEventListener(event, handler) { listeners.set(event, handler); },
      querySelectorAll: () => [],
    });
  }
  return {
    elements,
    document: {
      hidden: false,
      getElementById(id) {
        assert.ok(elements.has(id), '脚本引用的 HTML 元素必须存在：' + id);
        return elements.get(id);
      },
      addEventListener() {},
    },
  };
}
const sampleDevices = [
  {busid: '1-8.3', vidpid: '1241:e001', description: 'LYFdog : Sample',
    alias: '算王1', remark: '财务专用', binding: 'group', bindingLabel: '同型号第 1 台',
    duplicateModel: true, shared: true, connections: [{name: '前台电脑', address: '192.168.1.21', lastSeen: 1}]},
  {busid: '2-1', vidpid: '4321:abcd', description: 'Scanner',
    alias: '', remark: '', binding: 'serial', bindingLabel: '序列号 SN-42',
    duplicateModel: false, shared: false, connections: []},
];
async function settle() {
  await new Promise(resolve => setImmediate(resolve));
  await new Promise(resolve => setImmediate(resolve));
}
async function run(options = {}) {
  const opts = {storedToken: 'valid.token', authorized: true, devices: sampleDevices, ...options};
  const dom = makeDom();
  const store = new Map();
  if (opts.storedToken !== null) store.set('usbip-share-token', opts.storedToken);
  if (opts.legacyToken) store.set('fnos-usbip-token', opts.legacyToken);
  const calls = [];
  const intervals = [];
  const alerts = [];
  const sandbox = {
    document: dom.document,
    localStorage: {
      getItem: key => store.get(key) ?? null,
      setItem: (key, value) => store.set(key, String(value)),
      removeItem: key => store.delete(key),
    },
    fetch: async (url, init) => {
      calls.push({url, init});
      if (url === '/api/session') {
        if (opts.sessionHandler) return opts.sessionHandler();
        return response({ok: true, authorized: opts.authorized, mustChange: Boolean(opts.mustChange)});
      }
      if (url === '/api/devices') {
        if (opts.deviceHandler) return opts.deviceHandler();
        return response({ok: true, usbipPort: 5555, devices: opts.devices});
      }
      if (opts.mutationHandler) return opts.mutationHandler(url, init);
      return response({ok: false, error: '未实现的测试接口'}, 404);
    },
    setTimeout: () => 1,
    clearTimeout() {},
    setInterval: callback => { intervals.push(callback); return 1; },
    alert: message => alerts.push(message),
    confirm: () => true,
    console,
  };
  sandbox.window = sandbox;
  sandbox.location = {reload() {}};
  vm.createContext(sandbox);
  vm.runInContext(script, sandbox, {filename: 'index.html:inline'});
  await settle();
  return {opts, dom, store, calls, intervals, alerts, sandbox,
    el: id => dom.document.getElementById(id),
    evaluate: code => vm.runInContext(code, sandbox),
    deviceCalls: () => calls.filter(call => call.url === '/api/devices').length};
}
(async () => {
  console.log('[A] 登录与会话恢复');
  let r = await run({storedToken: null, authorized: false});
  check('无令牌显示登录框并隐藏主界面', !r.el('loginModal').hidden && r.el('mainApp').hidden);
  check('无令牌不请求接口', r.calls.length === 0);
  r = await run();
  check('有效令牌直接进入主界面', r.el('loginModal').hidden && !r.el('mainApp').hidden);
  check('有效令牌携带认证头拉取设备', r.deviceCalls() === 1 && r.calls[1].init.headers['X-Admin-Token'] === 'valid.token');
  check('有效令牌保留', r.store.get('usbip-share-token') === 'valid.token');
  check('绑定序号和同型号警告保持', r.el('content').innerHTML.includes('同型号第 1 台') && r.el('content').innerHTML.includes('无法区分个体'));
  check('统计以全部设备为准', r.el('totalCount').textContent === 2 && r.el('sharedCount').textContent === 1 && r.el('connectedCount').textContent === 1);
  r = await run({authorized: false});
  check('明确未授权才清除令牌并显示登录框', !r.store.has('usbip-share-token') && !r.el('loginModal').hidden);
  r = await run({sessionHandler: () => response({ok: false}, 401)});
  check('会话401清除令牌', !r.store.has('usbip-share-token'));
  r = await run({storedToken: null, legacyToken: 'legacy.token'});
  check('旧令牌迁移后删除旧键', r.store.get('usbip-share-token') === 'legacy.token' && !r.store.has('fnos-usbip-token'));

  console.log('[B] 网络与服务端故障');
  for (const [name, sessionHandler] of [
    ['断网', () => { throw new Error('offline'); }],
    ['服务器错误', () => response({ok: false}, 503)],
    ['响应损坏', () => ({ok: true, status: 200, json: async () => { throw new Error('invalid JSON'); }})],
    ['授权字段缺失', () => response({ok: true})],
  ]) {
    r = await run({sessionHandler});
    check(name + '不清除令牌或跳登录', r.store.has('usbip-share-token') && r.el('loginModal').hidden);
    check(name + '显示可重试提示', !r.el('errorBanner').hidden && !r.el('mainApp').hidden);
  }
  r.opts.sessionHandler = null;
  await r.el('retryBtn').onclick();
  check('网络恢复后重试自动恢复列表', r.deviceCalls() === 1 && r.el('errorBanner').hidden);
  const oldHtml = r.el('content').innerHTML;
  r.opts.deviceHandler = () => { throw new Error('offline'); };
  await r.evaluate('loadDevices()');
  check('设备刷新失败保留表格及令牌', r.el('content').innerHTML === oldHtml && r.store.has('usbip-share-token'));
  check('刷新失败恢复按钮并展示错误', !r.el('refreshTop').disabled && !r.el('errorBanner').hidden);
  r.opts.deviceHandler = () => response({ok: false}, 401);
  await r.evaluate('loadDevices()');
  check('设备接口401清除令牌', !r.store.has('usbip-share-token') && !r.el('loginModal').hidden);

  console.log('[C] 搜索与状态筛选');
  r = await run();
  for (const query of ['财务', '1-8.3', '1241:E001', '前台电脑', '192.168.1.21', 'lyfdog', '同型号第 1 台']) {
    r.el('deviceSearch').value = query;
    r.el('deviceSearch').listeners.get('input')();
    check('搜索字段：' + query, r.el('content').innerHTML.includes('算王1') && !r.el('content').innerHTML.includes('Scanner'));
  }
  r.el('deviceSearch').value = '';
  r.el('statusFilter').value = 'unshared';
  r.el('statusFilter').listeners.get('change')();
  check('筛选未共享设备', r.el('content').innerHTML.includes('Scanner') && !r.el('content').innerHTML.includes('算王1'));
  check('筛选不改变总数统计', r.el('totalCount').textContent === 2 && r.el('count').textContent === '显示 1 / 2 个');
  r.el('statusFilter').value = 'shared';
  r.el('statusFilter').listeners.get('change')();
  check('筛选共享设备', r.el('content').innerHTML.includes('算王1') && !r.el('content').innerHTML.includes('Scanner'));
  r.el('statusFilter').value = 'connected';
  r.el('statusFilter').listeners.get('change')();
  check('筛选有登记连接设备', r.el('content').innerHTML.includes('前台电脑') && !r.el('content').innerHTML.includes('Scanner'));
  r.el('deviceSearch').value = '没有这个设备';
  r.el('deviceSearch').listeners.get('input')();
  check('搜索无结果有独立空状态', r.el('content').innerHTML.includes('没有匹配的设备'));
  r.el('clearFilters').onclick();
  check('清除筛选恢复完整列表', r.el('deviceSearch').value === '' && r.el('statusFilter').value === 'all' && r.el('count').textContent === '显示 2 / 2 个');
  r = await run({devices: []});
  check('真正无设备有独立空状态', r.el('content').innerHTML.includes('没有发现 USB 设备'));

  console.log('[D] 防闪烁、去重与编辑保护');
  r = await run();
  let writes = r.el('content').writes;
  await r.evaluate('loadDevices()');
  check('数据不变不重绘表格', r.el('content').writes === writes);
  r.opts.devices = sampleDevices.map(device => ({...device, connections: device.connections.map(connection => ({...connection, lastSeen: 99}))}));
  await r.evaluate('loadDevices()');
  check('只有心跳时间变化不重绘表格', r.el('content').writes === writes);
  r.el('content').scrollLeft = 217;
  r.opts.devices = r.opts.devices.map(device => ({...device, remark: '新备注'}));
  await r.evaluate('loadDevices()');
  check('实际数据变化更新内容并保留横向滚动', r.el('content').writes === writes + 1 && r.el('content').scrollLeft === 217);
  let resolveRequest;
  r.opts.deviceHandler = () => new Promise(resolve => { resolveRequest = resolve; });
  const first = r.evaluate('loadDevices()');
  let requestCount = r.deviceCalls();
  await r.evaluate('loadDevices()');
  check('刷新中重复请求合并且按钮禁用', r.deviceCalls() === requestCount && r.el('refreshTop').disabled);
  check('刷新中不插入加载占位替换表格', r.el('content').innerHTML.includes('新备注'));
  r.evaluate("openEditor('1-8.3', currentDevices)");
  r.el('aliasInput').value = '正在输入的名称';
  writes = r.el('content').writes;
  resolveRequest(response({ok: true, usbipPort: 5555, devices: sampleDevices}));
  await first;
  check('编辑中在途响应不重绘或覆盖输入', r.el('content').writes === writes && r.el('aliasInput').value === '正在输入的名称');
  requestCount = r.deviceCalls();
  r.intervals[0]();
  await settle();
  check('编辑弹窗期间暂停自动轮询', r.deviceCalls() === requestCount);
  r.el('cancelEdit').onclick();
  check('取消编辑恢复渲染但不保存草稿', r.el('editorModal').hidden && r.el('content').innerHTML.includes('财务专用') && !r.el('content').innerHTML.includes('正在输入的名称'));
  r.dom.document.hidden = true;
  r.intervals[0]();
  await settle();
  check('后台标签页暂停轮询', r.deviceCalls() === requestCount);
  r.dom.document.hidden = false;
  r.opts.deviceHandler = null;
  r.intervals[0]();
  await settle();
  check('回到页面后恢复轮询', r.deviceCalls() === requestCount + 1);
  r = await run({mustChange: true});
  check('首次改密弹窗显示且暂停设备请求', !r.el('changeModal').hidden && r.deviceCalls() === 0);
  r.intervals[0]();
  await settle();
  check('强制改密时不轮询', r.deviceCalls() === 0);

  console.log('[E] 内容转义与原有管理操作');
  r = await run({devices: [{...sampleDevices[0], alias: '<img src=x onerror=alert(1)>', remark: '<script>bad</script>', binding: '未知绑定', bindingLabel: '<b>绑定</b>', connections: [{name: '<svg onload=bad()>', address: '" onmouseover="bad'}]}]});
  const html = r.el('content').innerHTML;
  check('名称、备注、绑定、连接方均转义', !html.includes('<img') && !html.includes('<script>bad') && !html.includes('<svg') && html.includes('&lt;b&gt;绑定&lt;/b&gt;'));
  check('未知绑定类型使用默认样式', html.includes('bind bind-none'));
  check('保留编辑、停止共享和强制断开操作', html.includes('data-action="edit"') && html.includes('data-action="unshare"') && html.includes('data-action="kick"'));
  r = await run();
  r.opts.mutationHandler = () => response({ok: true, message: '操作完成'});
  r.sandbox.testButton = {disabled: false};
  await r.evaluate("changeState('1-8.3', 'kick', testButton)");
  check('强制断开成功后恢复按钮可再次使用', !r.sandbox.testButton.disabled && r.calls.some(call => call.url === '/api/devices/1-8.3/kick'));
  const callCount = r.calls.length;
  r.sandbox.confirm = () => false;
  await r.evaluate("changeState('1-8.3', 'kick', testButton)");
  check('取消强制断开不发送请求', r.calls.length === callCount);
  r.evaluate("openEditor('1-8.3', currentDevices)");
  r.el('aliasInput').value = '新名称';
  r.el('remarkInput').value = '新备注';
  await r.evaluate('saveMetadata()');
  const saved = r.calls.find(call => call.url.endsWith('/metadata'));
  check('编辑名称备注继续保存原接口', saved && JSON.parse(saved.init.body).alias === '新名称' && JSON.parse(saved.init.body).remark === '新备注');
  check('保存后关闭编辑框并恢复按钮', r.el('editorModal').hidden && !r.el('saveEdit').disabled);

  const passed = results.filter(Boolean).length;
  console.log('\n总计: ' + passed + '/' + results.length + ' 通过');
  process.exitCode = passed === results.length ? 0 : 1;
})().catch(error => { console.error(error); process.exitCode = 1; });
