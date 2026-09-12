
    const TOKEN_KEY = 'usbip-share-token';
    // 项目改名前的旧键名，仅用于一次性迁移；迁移完即删除。
    const LEGACY_TOKEN_KEY = 'fnos-usbip-token';
    let token = localStorage.getItem(TOKEN_KEY) || localStorage.getItem(LEGACY_TOKEN_KEY) || '';
    if (token) localStorage.setItem(TOKEN_KEY, token);
    localStorage.removeItem(LEGACY_TOKEN_KEY);
    let mustChange = false;

    const mainApp = document.getElementById('mainApp');
    const loginModal = document.getElementById('loginModal');
    const changeModal = document.getElementById('changeModal');
    const editorModal = document.getElementById('editorModal');
    const aliasInput = document.getElementById('aliasInput');
    const remarkInput = document.getElementById('remarkInput');
    let editingBusId = '';

    document.getElementById('loginBtn').onclick = doLogin;
    document.getElementById('loginPass').addEventListener('keydown', e => { if (e.key === 'Enter') doLogin(); });
    document.getElementById('changeBtn').onclick = doChange;
    document.getElementById('changePassBtn').onclick = () => showChange();
    document.getElementById('logoutBtn').onclick = () => {
      token = ''; localStorage.removeItem(TOKEN_KEY);
      window.location.reload();
    };
    document.getElementById('refreshTop').onclick = loadDevices;
    document.getElementById('cancelEdit').onclick = () => { editorModal.hidden = true; };
    editorModal.onclick = e => { if (e.target === editorModal) editorModal.hidden = true; };
    document.addEventListener('keydown', e => { if (e.key === 'Escape') { editorModal.hidden = true; } });
    document.getElementById('saveEdit').onclick = saveMetadata;

    function escapeHtml(value) {
      return String(value ?? '').replace(/[&<>'"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;',"'":'&#39;','"':'&quot;'}[c]));
    }
    function flash(message) {
      document.getElementById('updated').textContent = message;
      setTimeout(() => { document.getElementById('updated').textContent = ''; }, 2600);
    }
    function apiHeaders() {
      const headers = {'Accept': 'application/json'};
      if (token) headers['X-Admin-Token'] = token;
      return headers;
    }
    function jsonHeaders() {
      return Object.assign({'Content-Type': 'application/json'}, apiHeaders());
    }
    function showLogin() {
      mainApp.hidden = true;
      loginModal.hidden = false;
      document.getElementById('loginPass').focus();
    }
    function showChange(force) {
      changeModal.hidden = false;
      if (force) document.getElementById('changeHint').textContent = '出于安全要求，请先修改默认密码后再使用。';
      else document.getElementById('changeHint').textContent = '修改后需要重新登录。';
      document.getElementById('oldPass').focus();
    }
    async function doLogin() {
      const password = document.getElementById('loginPass').value;
      const button = document.getElementById('loginBtn');
      button.disabled = true;
      try {
        const response = await fetch('/api/login', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({password})});
        const data = await response.json();
        if (!response.ok || !data.ok) throw new Error(data.error || '登录失败');
        token = data.token;
        mustChange = Boolean(data.mustChange);
        localStorage.setItem(TOKEN_KEY, token);
        loginModal.hidden = true;
        mainApp.hidden = false;
        document.getElementById('loginPass').value = '';
        if (mustChange) showChange(true);
        await loadDevices();
      } catch (error) {
        alert(error.message);
      } finally {
        button.disabled = false;
      }
    }
    async function doChange() {
      const oldPassword = document.getElementById('oldPass').value;
      const newPassword = document.getElementById('newPass').value;
      const newPassword2 = document.getElementById('newPass2').value;
      if (newPassword !== newPassword2) { alert('两次输入的新密码不一致'); return; }
      if (newPassword.length < 6) { alert('新密码至少 6 个字符'); return; }
      const button = document.getElementById('changeBtn');
      button.disabled = true;
      try {
        const response = await fetch('/api/change-password', {method: 'POST', headers: jsonHeaders(), body: JSON.stringify({oldPassword, newPassword})});
        const data = await response.json();
        if (!response.ok || !data.ok) throw new Error(data.error || '修改失败');
        mustChange = false;
        changeModal.hidden = true;
        document.getElementById('oldPass').value = '';
        document.getElementById('newPass').value = '';
        document.getElementById('newPass2').value = '';
        flash('密码已修改，下次登录请使用新密码');
      } catch (error) {
        alert(error.message);
      } finally {
        button.disabled = false;
      }
    }
    async function loadDevices() {
      if (!token) { showLogin(); return; }
      const content = document.getElementById('content');
      content.innerHTML = '<div class="empty">正在读取 USB 设备…</div>';
      try {
        const response = await fetch('/api/devices', {cache: 'no-store', headers: apiHeaders()});
        if (response.status === 401) { showLogin(); return; }
        const data = await response.json();
        if (!response.ok || !data.ok) throw new Error(data.error || '读取设备失败');
        document.getElementById('port').textContent = data.usbipPort;
        document.getElementById('count').textContent = `共 ${data.devices.length} 个`;
        renderDevices(data.devices);
        document.getElementById('updated').textContent = '刚刚更新';
      } catch (error) {
        document.getElementById('count').textContent = '';
        content.innerHTML = `<div class="error">${escapeHtml(error.message)}<br><br><button class="secondary" onclick="loadDevices()">重试</button></div>`;
      }
    }
    function connectionSummary(connections) {
      if (!connections || !connections.length) return '<span class="muted">未登记连接方</span>';
      return connections.map(connection => {
        const name = escapeHtml(connection.name || '未命名客户端');
        const address = escapeHtml(connection.address || '地址未知');
        return `<div class="owner"><strong>${name}</strong><small>${address}</small></div>`;
      }).join('');
    }
    function renderDevices(devices) {
      const content = document.getElementById('content');
      if (!devices.length) {
        content.innerHTML = '<div class="empty">没有发现 USB 设备。请确认设备已插入，并检查容器的 USB 映射。</div>';
        return;
      }
      const rows = devices.map(device => {
        const shared = Boolean(device.shared);
        const kick = shared
          ? `<button class="danger" data-action="kick" data-busid="${escapeHtml(device.busid)}">强制断开连接</button>`
          : '';
        const action = shared
          ? `<button class="danger" data-action="unshare" data-busid="${escapeHtml(device.busid)}">停止共享</button>${kick}`
          : `<button class="primary" data-action="share" data-busid="${escapeHtml(device.busid)}">开始共享</button>`;
        const alias = device.alias ? `<div class="device-name">${escapeHtml(device.alias)}</div><div class="device-sub">系统名称：${escapeHtml(device.description || '未知')}</div>`
          : `<div class="device-name">${escapeHtml(device.description || '未知 USB 设备')}</div>`;
        // 文案由服务端给出（含真实序号），这里只负责配色；未知取值回退到“未设置”。
        const bindStyles = {
          serial: 'bind-serial', model: 'bind-model', group: 'bind-group',
          legacy: 'bind-legacy', none: 'bind-none'
        };
        const bindStyle = bindStyles[device.binding] || 'bind-none';
        const bindText = device.bindingLabel || '未设置';
        const bindBadge = `<span class="bind ${bindStyle}" title="名称/备注的识别依据">${escapeHtml(bindText)}</span>`;
        const dupHint = device.duplicateModel
          ? '<div class="device-sub warn-text">同型号多台且无序列号，无法区分个体</div>'
          : '';
        return `<tr>
          <td>${alias}<div class="device-sub">VID/PID：${escapeHtml(device.vidpid)}</div>${dupHint}</td>
          <td class="remark">${device.remark ? escapeHtml(device.remark) : '<span class="muted">未设置</span>'}</td>
          <td><code>${escapeHtml(device.busid)}</code></td>
          <td>${bindBadge}</td>
          <td>${connectionSummary(device.connections)}</td>
          <td><span class="badge ${shared ? 'shared' : 'free'}">${shared ? '● 已共享' : '○ 未共享'}</span></td>
          <td><div class="actions"><button class="secondary" data-action="edit" data-busid="${escapeHtml(device.busid)}">编辑名称/备注</button>${action}</div></td>
        </tr>`;
      }).join('');
      content.innerHTML = `<table><thead><tr><th>设备（显示名称）</th><th>设备备注</th><th>Bus ID</th><th>名称绑定依据</th><th>连接方</th><th>状态</th><th>操作</th></tr></thead><tbody>${rows}</tbody></table>`;
      content.querySelectorAll('button[data-action]').forEach(button => {
        button.onclick = () => {
          if (button.dataset.action === 'edit') openEditor(button.dataset.busid, devices);
          else changeState(button.dataset.busid, button.dataset.action, button);
        };
      });
    }
    function openEditor(busid, devices) {
      const device = devices.find(item => item.busid === busid);
      if (!device) return;
      editingBusId = busid;
      aliasInput.value = device.alias || '';
      remarkInput.value = device.remark || '';
      editorModal.hidden = false;
      aliasInput.focus();
    }
    async function saveMetadata() {
      if (!editingBusId) return;
      const saveButton = document.getElementById('saveEdit');
      saveButton.disabled = true;
      try {
        const response = await fetch(`/api/devices/${encodeURIComponent(editingBusId)}/metadata`, {
          method: 'POST', headers: jsonHeaders(),
          body: JSON.stringify({alias: aliasInput.value, remark: remarkInput.value})
        });
        const data = await response.json();
        if (!response.ok || !data.ok) throw new Error(data.error || data.message || '保存失败');
        editorModal.hidden = true;
        flash('设备名称和备注已保存');
        await loadDevices();
      } catch (error) {
        alert(error.message);
      } finally {
        saveButton.disabled = false;
      }
    }
    async function changeState(busid, action, button) {
      if (action === 'kick' && !window.confirm('确定要从服务器强制断开此设备的远程客户端吗？设备会继续保持共享，设置了自动重连的客户端可能再次连接。')) return;
      button.disabled = true;
      try {
        const response = await fetch(`/api/devices/${encodeURIComponent(busid)}/${action}`, {
          method: 'POST', headers: apiHeaders()
        });
        const data = await response.json();
        if (!response.ok || !data.ok) throw new Error(data.error || data.message || '操作失败');
        flash(data.message || '操作成功');
        await loadDevices();
      } catch (error) {
        button.disabled = false;
        alert(error.message);
      }
    }
    async function bootstrap() {
      // 登录框只在“登录成功”时被隐藏的旧逻辑，会让带着有效登录态的刷新一直
      // 把密码框挂在屏幕上。这里改为先问服务端这个令牌是否仍然有效，再决定
      // 显示设备列表还是登录框。
      if (!token) { showLogin(); return; }
      let data = null;
      try {
        const response = await fetch('/api/session', {cache: 'no-store', headers: apiHeaders()});
        data = await response.json();
        if (!response.ok || !data.ok || !data.authorized) data = null;
      } catch (error) {
        data = null;
      }
      if (!data) {
        // 令牌过期或被改密码废止：清掉本地令牌，让用户重新登录。
        token = '';
        localStorage.removeItem(TOKEN_KEY);
        showLogin();
        return;
      }
      mustChange = Boolean(data.mustChange);
      loginModal.hidden = true;
      mainApp.hidden = false;
      if (mustChange) showChange(true);
      await loadDevices();
    }
    bootstrap().catch(() => showLogin());
    setInterval(() => { if (!loginModal.hidden) return; if (!changeModal.hidden && mustChange) return; loadDevices(); }, 10000);
  