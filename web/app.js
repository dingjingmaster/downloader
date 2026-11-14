(() => {
  const pageSize = 20;
  let page = 1;
  let pages = 1;
  const stateLabels = new Map([
    ["created", "待开始"],
    ["queued", "排队中"],
    ["downloading", "下载中"],
    ["paused", "已暂停"],
    ["completed", "已完成"],
    ["failed", "下载失败"],
    ["cancelled", "已取消"]
  ]);
  const escapeHtml = (value) => String(value).replace(/[&<>"']/g, char => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
  }[char]));

  const copyText = async (value) => {
    if (navigator.clipboard && window.isSecureContext) {
      try {
        await navigator.clipboard.writeText(value);
        return;
      } catch (error) {
        // Fall through for browsers that reject the asynchronous clipboard API.
      }
    }
    const textarea = document.createElement("textarea");
    textarea.value = value;
    textarea.setAttribute("readonly", "");
    textarea.style.position = "fixed";
    textarea.style.top = "0";
    textarea.style.opacity = "0";
    document.body.append(textarea);
    textarea.select();
    let copied = false;
    try {
      copied = document.execCommand("copy");
    } finally {
      textarea.remove();
    }
    if (!copied) throw new Error("复制链接失败");
  };

  let statusTimer = null;
  const showStatus = (message) => {
    const status = document.querySelector("#action-status");
    status.textContent = message;
    clearTimeout(statusTimer);
    statusTimer = setTimeout(() => { status.textContent = ""; }, 2000);
  };

  const formatBytes = (value) => {
    if (!value) return "0 B";
    const units = ["B", "KiB", "MiB", "GiB", "TiB"];
    let index = 0;
    let number = value;
    while (number >= 1024 && index < units.length - 1) {
      number /= 1024;
      index += 1;
    }
    return `${number.toFixed(index ? 1 : 0)} ${units[index]}`;
  };

  let csrfToken = "";
  const request = async (url, options = {}) => {
    const headers = { "Content-Type": "application/json", ...(options.headers || {}) };
    if (csrfToken) headers["X-CSRF-Token"] = csrfToken;
    const response = await fetch(url, { ...options, headers });
    const body = await response.json().catch(() => ({ ok: false, error: "请求失败" }));
    if (response.status === 401) { showLogin(); throw new Error(body.error || "需要登录"); }
    if (!response.ok || !body.ok) throw new Error(body.error || "请求失败");
    return body;
  };

  const render = (body) => {
    pages = Math.max(1, body.pages || 1);
    document.querySelector("#page-label").textContent = `${body.page} / ${pages}`;
    document.querySelector("#summary").textContent = `共 ${body.total} 个任务`;
    document.querySelector("#previous").disabled = page <= 1;
    document.querySelector("#next").disabled = page >= pages;
    const groups = new Map((body.groups || []).map(group => [group.id, group]));
    const rows = [];
    let lastGroup = null;
    body.tasks.forEach((task) => {
      const groupId = task.group_id || "";
      if (groupId && groupId !== lastGroup) {
        const group = groups.get(groupId) || { id: groupId, name: task.group_name || "分组", task_count: 0,
          completed: 0, failed: 0, total_bytes: 0, downloaded_bytes: 0 };
        rows.push(`<tr class="group-row"><td colspan="7">
          <strong>${escapeHtml(group.name || "分组")}</strong>
          <span class="task-note">${Number(group.completed)} / ${Number(group.task_count)} 完成${Number(group.failed) ? `，${Number(group.failed)} 失败` : ""}</span>
          <span class="task-note">${formatBytes(group.downloaded_bytes)} / ${group.total_bytes ? formatBytes(group.total_bytes) : "未知"}</span>
          <span class="task-actions">
            <button data-group-action="start" data-group="${escapeHtml(groupId)}">开始整组</button>
            <button data-group-action="pause" data-group="${escapeHtml(groupId)}">暂停整组</button>
            <button data-group-action="delete" data-group="${escapeHtml(groupId)}">删除整组</button>
          </span>
        </td></tr>`);
      }
      lastGroup = groupId;
      rows.push(`<tr>
      <td title="${escapeHtml(task.resource_url)}">${escapeHtml(task.file_name)}</td>
      <td>${escapeHtml(task.protocol)}</td>
      <td>${escapeHtml(stateLabels.get(task.state) || "未知状态")}${task.state === 'completed' && task.error_message ? '（同步警告）' : ''}
        ${task.retry_count ? `<div class="task-note">重试 ${Number(task.retry_count)} / ${Number(task.max_retries)}</div>` : ''}
        ${task.error_message ? `<div class="task-error">${escapeHtml(task.error_message)}</div>` : ''}
        ${Number(task.duration_seconds) > 0 ? `<div class="task-note">录制 ${Number(task.duration_seconds).toFixed(1)} 秒${task.gap_count ? `，缺口 ${Number(task.gap_count)}` : ''}</div>` : ''}
      </td>
      <td>${task.total_bytes || task.state === 'completed' ? Number(task.progress).toFixed(1) + '%' : '—'}</td>
      <td>${task.speed_bytes_sec ? formatBytes(task.speed_bytes_sec) + '/s' : '—'}</td>
      <td>${formatBytes(task.downloaded_bytes)} / ${task.total_bytes ? formatBytes(task.total_bytes) : '未知'}</td>
      <td><div class="task-actions">
        <button data-action="start" data-id="${escapeHtml(task.id)}">开始</button>
        <button data-action="pause" data-id="${escapeHtml(task.id)}">暂停</button>
        ${(task.protocol === 'hls' || task.protocol === 'dash') && Number(task.duration_seconds) > 0 ? `<button data-action="finish" data-id="${escapeHtml(task.id)}">结束录制</button>` : ''}
        ${task.resource_url ? `<button data-action="copy" data-url="${escapeHtml(task.resource_url)}">复制链接</button>` : ''}
        <button data-action="delete" data-id="${escapeHtml(task.id)}">删除</button>
      </div>
      </td>
    </tr>`);
    });
    document.querySelector("#tasks").innerHTML = rows.join("") || '<tr><td colspan="7" class="empty-state">暂无下载任务，点击「新建任务」添加链接</td></tr>';
  };

  const load = async () => {
    try { render(await request(`/api/tasks?page=${page}&page_size=${pageSize}`)); }
    catch (error) { document.querySelector("#summary").textContent = error.message; }
  };

  const dialog = document.querySelector("#create-dialog");
  const directoryDialog = document.querySelector("#directory-dialog");
  const baiduOptions = dialog.querySelector("#baidu-options");
  const urlInput = dialog.querySelector('[name="url"]');
  const refreshBaiduOptions = () => {
    const value = (urlInput.value || "").trim().toLowerCase();
    const enabled = value.startsWith("baidu://") || value.includes("://pan.baidu.com/") ||
      value.includes("://yun.baidu.com/") || value.includes("://eyun.baidu.com/");
    baiduOptions.hidden = !enabled;
  };
  urlInput.addEventListener("input", refreshBaiduOptions);
  const directoryInput = dialog.querySelector('[name="target_directory"]');
  let directoryPath = "";
  let directoryParent = "";
  let directoryNames = [];
  const showHiddenDirectories = document.querySelector("#show-hidden-directories");
  const renderDirectories = () => {
    const list = document.querySelector("#directory-list");
    list.replaceChildren();
    const visible = directoryNames.filter(name => showHiddenDirectories.checked || !name.startsWith("."));
    visible.sort((a, b) => a.localeCompare(b)).forEach(name => {
      const button = document.createElement("button");
      button.type = "button";
      button.textContent = `▸ ${name}`;
      button.addEventListener("click", () => browseDirectory(`${directoryPath.replace(/\/$/, "")}/${name}`));
      list.append(button);
    });
    if (!visible.length) list.textContent = "没有可显示的子文件夹";
  };
  showHiddenDirectories.addEventListener("change", renderDirectories);
  const browseDirectory = async (path) => {
    const select = document.querySelector("#directory-select");
    const list = document.querySelector("#directory-list");
    const up = document.querySelector("#directory-up");
    select.disabled = up.disabled = true;
    showHiddenDirectories.disabled = true;
    directoryNames = [];
    list.replaceChildren();
    document.querySelector("#directory-error").textContent = "";
    try {
      const body = await request("/api/directories", { method: "POST", body: JSON.stringify({ path }) });
      directoryPath = body.path;
      directoryParent = body.parent;
      document.querySelector("#directory-path").textContent = directoryPath;
      directoryNames = body.directories;
      renderDirectories();
      showHiddenDirectories.disabled = false;
      select.disabled = false;
      up.disabled = directoryParent === directoryPath;
    } catch (error) {
      document.querySelector("#directory-error").textContent = error.message;
      // Keep navigation available if a child directory cannot be opened.
      up.disabled = !directoryPath;
    }
  };
  document.querySelector("#choose-directory").addEventListener("click", () => {
    directoryPath = directoryParent = "";
    showHiddenDirectories.checked = false;
    document.querySelector("#directory-path").textContent = "";
    directoryDialog.showModal();
    browseDirectory(directoryInput.value || "/");
  });
  document.querySelector("#reset-directory").addEventListener("click", () => { directoryInput.value = ""; });
  document.querySelector("#directory-up").addEventListener("click", () => browseDirectory(directoryParent || "/"));
  document.querySelector("#directory-cancel").addEventListener("click", () => directoryDialog.close());
  document.querySelector("#directory-select").addEventListener("click", () => {
    directoryInput.value = directoryPath;
    directoryDialog.close();
  });
  document.querySelector("#new-task").addEventListener("click", () => {
    document.querySelector("#create-error").textContent = "";
    dialog.showModal();
    dialog.querySelector('[name="url"]').focus();
  });
  document.querySelector("#cancel-create").addEventListener("click", () => dialog.close());
  document.querySelector("#create-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    const values = Object.fromEntries(new FormData(event.target));
    values.url = values.url.trim();
    values.target_directory = values.target_directory.trim();
    const http = {};
    if ((values.http_user_agent || '').trim()) http.user_agent = values.http_user_agent.trim();
    if ((values.http_referer || '').trim()) http.referer = values.http_referer.trim();
    delete values.http_user_agent;
    delete values.http_referer;
    const extra = {};
    if (Object.keys(http).length) extra.http = http;
    const baidu = {};
    if ((values.baidu_pwd || '').trim()) baidu.pwd = values.baidu_pwd.trim();
    if ((values.baidu_path || '').trim()) baidu.path = values.baidu_path.trim();
    delete values.baidu_pwd;
    delete values.baidu_path;
    if (Object.keys(baidu).length) extra.baidu = baidu;
    if (values.auth_profile) extra.auth_profile = values.auth_profile;
    if (values.proxy_profile) extra.proxy_profile = values.proxy_profile;
    delete values.auth_profile;
    delete values.proxy_profile;
    if (Object.keys(extra).length) values.extra = extra;
    const submit = event.target.querySelector('[type="submit"]');
    submit.disabled = true;
    try {
      const created = await request("/api/tasks", { method: "POST", body: JSON.stringify(values) });
      if (created.group && created.group.truncated) {
        alert(`已创建 ${created.group.task_count} 条任务，请在列表中分页查看。`);
      }
      event.target.reset();
      refreshBaiduOptions();
      dialog.close();
      page = 1;
      await load();
    } catch (error) {
      document.querySelector("#create-error").textContent = error.message;
    } finally {
      submit.disabled = false;
    }
  });
  document.querySelector("#clear-completed").addEventListener("click", async () => {
    const button = document.querySelector("#clear-completed");
    button.disabled = true;
    document.querySelector("#action-error").textContent = '';
    try {
      await request("/api/tasks/clear-completed", { method: "POST" });
      await load();
    } catch (error) {
      document.querySelector("#action-error").textContent = error.message;
    } finally {
      button.disabled = false;
    }
  });
  document.querySelector("#previous").addEventListener("click", () => { if (page > 1) { page -= 1; load(); } });
  document.querySelector("#next").addEventListener("click", () => { if (page < pages) { page += 1; load(); } });
  document.querySelector("#tasks").addEventListener("click", async (event) => {
    const button = event.target.closest("button");
    if (!button) return;
    if (button.dataset.groupAction) {
      const groupId = encodeURIComponent(button.dataset.group);
      const groupAction = button.dataset.groupAction;
      button.disabled = true;
      document.querySelector("#action-error").textContent = '';
      try {
        await request(groupAction === "delete" ? `/api/groups/${groupId}` : `/api/groups/${groupId}/${groupAction}`,
          { method: groupAction === "delete" ? "DELETE" : "POST" });
        await load();
      } catch (error) {
        document.querySelector("#action-error").textContent = error.message;
      } finally {
        button.disabled = false;
      }
      return;
    }
    const action = button.dataset.action;
    if (action === "copy") {
      document.querySelector("#action-error").textContent = '';
      try {
        await copyText(button.dataset.url || "");
        showStatus("已复制链接");
      } catch (error) {
        document.querySelector("#action-error").textContent = error.message;
      }
      return;
    }
    const id = encodeURIComponent(button.dataset.id);
    button.disabled = true;
    document.querySelector("#action-error").textContent = '';
    try {
      await request(action === "delete" ? `/api/tasks/${id}` : `/api/tasks/${id}/${action}`, { method: action === "delete" ? "DELETE" : "POST" });
      await load();
    } catch (error) {
      document.querySelector("#action-error").textContent = error.message;
    } finally {
      button.disabled = false;
    }
  });
  const loginDialog = document.querySelector("#login-dialog");
  const logoutButton = document.querySelector("#logout");
  const profilesDialog = document.querySelector("#profiles-dialog");
  let refreshTimer = null;
  const showLogin = () => {
    if (!loginDialog.open) loginDialog.showModal();
  };
  const fillProfiles = async () => {
    const authSelect = document.querySelector('[name="auth_profile"]');
    const proxySelect = document.querySelector('[name="proxy_profile"]');
    try {
      const auth = await request("/api/auth-profiles");
      const proxies = await request("/api/proxy-profiles");
      const fill = (select, values) => {
        const current = select.value;
        select.replaceChildren(new Option("不使用", ""));
        values.forEach(item => select.append(new Option(
          item.name + (item.username ? `（${item.username}）` : ""), item.name)));
        select.value = values.some(item => item.name === current) ? current : "";
      };
      fill(authSelect, auth.credentials);
      fill(proxySelect, proxies.proxies);
      const renderList = (container, items, kind) => {
        container.replaceChildren();
        items.forEach(item => {
          const row = document.createElement("div");
          row.className = "profile-row";
          const label = document.createElement("span");
          label.textContent = item.name + (item.username ? `（${item.username}）` : "");
          const remove = document.createElement("button");
          remove.type = "button";
          remove.textContent = "删除";
          remove.addEventListener("click", async () => {
            try {
              await request(`/api/${kind}/${encodeURIComponent(item.name)}`, { method: "DELETE" });
              await fillProfiles();
            } catch (error) {
              document.querySelector("#profile-error").textContent = error.message;
            }
          });
          row.append(label, remove);
          container.append(row);
        });
      };
      renderList(document.querySelector("#auth-profile-list"), auth.credentials, "auth-profiles");
      renderList(document.querySelector("#proxy-profile-list"), proxies.proxies, "proxy-profiles");
    } catch (error) {
      // Authentication may be disabled or the credential store unavailable.
    }
  };
  const start = async () => {
    logoutButton.hidden = !csrfToken;
    await load();
    await fillProfiles();
    if (!refreshTimer) refreshTimer = setInterval(load, 2000);
  };
  const bootstrap = async () => {
    try {
      const response = await fetch("/api/session");
      const body = await response.json();
      if (response.status === 401) { showLogin(); return; }
      csrfToken = body.auth ? body.csrf_token || "" : "";
      await start();
    } catch (error) {
      document.querySelector("#summary").textContent = error.message;
      showLogin();
    }
  };
  document.querySelector("#login-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    const values = Object.fromEntries(new FormData(event.target));
    try {
      const response = await fetch("/api/login", {
        method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(values)
      });
      const body = await response.json();
      if (!response.ok || !body.ok) throw new Error(body.error || "登录失败");
      csrfToken = body.csrf_token || "";
      event.target.reset();
      loginDialog.close();
      await start();
    } catch (error) {
      document.querySelector("#login-error").textContent = "用户名或密码错误";
    }
  });
  logoutButton.addEventListener("click", async () => {
    try { await request("/api/logout", { method: "POST" }); } catch (error) { /* already signed out */ }
    csrfToken = "";
    location.reload();
  });
  document.querySelector("#profiles").addEventListener("click", async () => {
    await fillProfiles();
    profilesDialog.showModal();
  });
  document.querySelector("#profiles-close").addEventListener("click", () => profilesDialog.close());
  document.querySelector("#auth-profile-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    const values = Object.fromEntries(new FormData(event.target));
    try {
      await request("/api/auth-profiles", { method: "POST", body: JSON.stringify(values) });
      event.target.reset();
      await fillProfiles();
      document.querySelector("#auth-profile-error").textContent = "";
    } catch (error) {
      document.querySelector("#auth-profile-error").textContent = error.message;
    }
  });
  document.querySelector("#proxy-profile-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    const values = Object.fromEntries(new FormData(event.target));
    try {
      await request("/api/proxy-profiles", { method: "POST", body: JSON.stringify(values) });
      event.target.reset();
      await fillProfiles();
      document.querySelector("#proxy-profile-error").textContent = "";
    } catch (error) {
      document.querySelector("#proxy-profile-error").textContent = error.message;
    }
  });
  bootstrap();
})();
