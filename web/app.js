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

  const request = async (url, options = {}) => {
    const response = await fetch(url, { headers: { "Content-Type": "application/json" }, ...options });
    const body = await response.json();
    if (!response.ok || !body.ok) throw new Error(body.error || "请求失败");
    return body;
  };

  const render = (body) => {
    pages = Math.max(1, body.pages || 1);
    document.querySelector("#page-label").textContent = `${body.page} / ${pages}`;
    document.querySelector("#summary").textContent = `共 ${body.total} 个任务`;
    document.querySelector("#previous").disabled = page <= 1;
    document.querySelector("#next").disabled = page >= pages;
    const rows = body.tasks.map((task) => `<tr>
      <td title="${escapeHtml(task.resource_url)}">${escapeHtml(task.file_name)}</td>
      <td>${escapeHtml(task.protocol)}</td>
      <td>${escapeHtml(stateLabels.get(task.state) || "未知状态")}${task.state === 'completed' && task.error_message ? '（同步警告）' : ''}
        ${task.retry_count ? `<div class="task-note">重试 ${Number(task.retry_count)} / ${Number(task.max_retries)}</div>` : ''}
        ${task.error_message ? `<div class="task-error">${escapeHtml(task.error_message)}</div>` : ''}
      </td>
      <td>${task.total_bytes || task.state === 'completed' ? Number(task.progress).toFixed(1) + '%' : '—'}</td>
      <td>${formatBytes(task.downloaded_bytes)} / ${task.total_bytes ? formatBytes(task.total_bytes) : '未知'}</td>
      <td><div class="task-actions">
        <button data-action="start" data-id="${escapeHtml(task.id)}">开始</button>
        <button data-action="pause" data-id="${escapeHtml(task.id)}">暂停</button>
        <button data-action="delete" data-id="${escapeHtml(task.id)}">删除</button>
      </div>
      </td>
    </tr>`).join("");
    document.querySelector("#tasks").innerHTML = rows || '<tr><td colspan="6" class="empty-state">暂无下载任务，点击「新建任务」添加链接</td></tr>';
  };

  const load = async () => {
    try { render(await request(`/api/tasks?page=${page}&page_size=${pageSize}`)); }
    catch (error) { document.querySelector("#summary").textContent = error.message; }
  };

  const dialog = document.querySelector("#create-dialog");
  const directoryDialog = document.querySelector("#directory-dialog");
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
    if (Object.keys(http).length) values.extra = { http };
    const submit = event.target.querySelector('[type="submit"]');
    submit.disabled = true;
    try {
      await request("/api/tasks", { method: "POST", body: JSON.stringify(values) });
      event.target.reset();
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
    const action = button.dataset.action;
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
  load();
  setInterval(load, 2000);
})();
