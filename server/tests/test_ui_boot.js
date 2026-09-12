// Runs the real inline script from index.html against a minimal DOM/fetch
// stub, to prove what the login box does on page load.
const fs = require('fs');
const vm = require('vm');
const path = require('path');

const script = fs.readFileSync(path.join(__dirname, 'inline.js'), 'utf8');
const results = [];
function check(name, cond, detail) {
  results.push(!!cond);
  console.log((cond ? '  PASS ' : '  FAIL ') + name + (detail ? '  [' + detail + ']' : ''));
}

function makeDom() {
  const elements = new Map();
  const el = (id) => {
    if (!elements.has(id)) {
      elements.set(id, {
        id, hidden: false, textContent: '', innerHTML: '', value: '',
        focus() {}, addEventListener() {}, querySelectorAll: () => [],
      });
    }
    return elements.get(id);
  };
  return {
    elements,
    document: {
      getElementById: el,
      addEventListener() {},
      querySelectorAll: () => [],
    },
  };
}

function makeContext({ storedToken, authorized, devices }) {
  const dom = makeDom();
  const store = new Map();
  if (storedToken !== null) store.set('usbip-share-token', storedToken);
  const calls = [];
  const sandbox = {
    document: dom.document,
    localStorage: {
      getItem: (k) => (store.has(k) ? store.get(k) : null),
      setItem: (k, v) => store.set(k, String(v)),
      removeItem: (k) => store.delete(k),
    },
    fetch: async (url) => {
      calls.push(url);
      if (url === '/api/session') {
        return { ok: true, status: 200, json: async () => ({ ok: true, authorized, mustChange: false }) };
      }
      if (url === '/api/devices') {
        return { ok: true, status: 200, json: async () => ({ ok: true, usbipPort: 5555, devices }) };
      }
      return { ok: false, status: 404, json: async () => ({ ok: false, error: 'nope' }) };
    },
    setTimeout: () => 0,
    setInterval: () => 0,
    alert: () => {},
    console,
    Boolean, Object, String, Number, JSON, Array, Error, Promise,
  };
  sandbox.window = sandbox;
  sandbox.globalThis = sandbox;
  return { sandbox, dom, store, calls };
}

const sampleDevices = [{
  busid: '1-8.3', vidpid: '1241:e001', description: 'LYFdog : Sample',
  alias: '算王1', remark: '', binding: 'group', bindingLabel: '同型号第 1 台',
  duplicateModel: true, shared: true, connections: [],
}];

async function run(name, opts) {
  const { sandbox, dom, store, calls } = makeContext(opts);
  vm.createContext(sandbox);
  vm.runInContext(script, sandbox, { filename: 'inline.js' });
  await new Promise((r) => setImmediate(r));
  await new Promise((r) => setImmediate(r));
  return { dom, store, calls, name };
}

(async () => {
  console.log('[A] 无令牌首次访问');
  let r = await run('no-token', { storedToken: null, authorized: false, devices: sampleDevices });
  check('登录框显示', r.dom.elements.get('loginModal').hidden === false);
  check('主界面隐藏', r.dom.elements.get('mainApp').hidden === true);

  console.log('[B] 带有效令牌刷新（本次修复的核心场景）');
  r = await run('valid', { storedToken: '1789831838.abc', authorized: true, devices: sampleDevices });
  check('登录框不再出现', r.dom.elements.get('loginModal').hidden === true);
  check('直接进入主界面', r.dom.elements.get('mainApp').hidden === false);
  check('确实拉取了设备列表', r.calls.includes('/api/devices'));
  check('令牌保留(未被误清)', r.store.get('usbip-share-token') === '1789831838.abc');
  const html = r.dom.elements.get('content').innerHTML;
  check('渲染出“同型号第 1 台”识别依据', html.includes('同型号第 1 台'), html.match(/bind [^"]*/) ? html.match(/bind [^"]*/)[0] : '');
  check('渲染出同型号提示', html.includes('无法区分个体'));

  console.log('[C] 令牌已失效（改过密码/过期）');
  r = await run('invalid', { storedToken: 'stale.token', authorized: false, devices: sampleDevices });
  check('要求重新登录', r.dom.elements.get('loginModal').hidden === false);
  check('失效令牌已从本地清除', !r.store.has('usbip-share-token'));

  console.log();
  const ok = results.filter(Boolean).length;
  console.log('总计: ' + ok + '/' + results.length + ' 通过');
  process.exit(ok === results.length ? 0 : 1);
})();
