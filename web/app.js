"use strict";

const $ = (id) => document.getElementById(id);
const defaultPreferences = {
  displayName: "",
  avatarTone: "auto",
  theme: "system",
  reduceMotion: false,
  enterSends: true
};
const systemDark = matchMedia("(prefers-color-scheme: dark)");
const avatarPalette = [221, 161, 263, 12, 188, 316];

const state = {
  preferences: defaultPreferences,
  name: null,
  epoch: 0,
  cursor: 0,
  view: "group",
  peer: null,
  group: [],
  private: new Map(),
  members: [],
  history: null,
  refresh: null,
  unread: new Map(),
  sendBusy: false,
  uploadBusy: false
};

const panels = {
  group: ["公共聊天室", "群聊消息与共享文件", "hash"],
  history: ["最近消息", "服务端保存的最近 20 条群聊和群文件", "history"],
  files: ["群文件", "从聊天记录中查看和下载文件", "file"]
};

const el = (tag, cls, text) => {
  const node = document.createElement(tag);
  if (cls) node.className = cls;
  if (text !== undefined) node.textContent = text;
  return node;
};

function icon(name) {
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  const use = document.createElementNS(svg.namespaceURI, "use");
  use.setAttribute("href", "#glyph-" + name);
  svg.setAttribute("aria-hidden", "true");
  svg.append(use);
  return svg;
}

function settingsKey(name) {
  return "linkroom:preferences:" + name;
}

function readPreferences(name) {
  if (!name) return defaultPreferences;
  let saved;
  try {
    saved = JSON.parse(localStorage.getItem(settingsKey(name)));
  } catch {
    return defaultPreferences;
  }
  if (!saved || typeof saved !== "object") return defaultPreferences;
  return {
    displayName: typeof saved.displayName === "string" ? saved.displayName.trim().slice(0, 24) : "",
    avatarTone: avatarPalette.map(String).includes(saved.avatarTone) ? saved.avatarTone : "auto",
    theme: ["system", "light", "dark"].includes(saved.theme) ? saved.theme : "system",
    reduceMotion: saved.reduceMotion === true,
    enterSends: saved.enterSends !== false
  };
}

function applyAppearance() {
  const {theme, reduceMotion} = state.preferences;
  const dark = theme === "dark" || (theme === "system" && systemDark.matches);
  document.documentElement.dataset.theme = dark ? "dark" : "light";
  document.documentElement.toggleAttribute("data-reduced-motion", reduceMotion);
  document.querySelector('meta[name="theme-color"]').content = dark ? "#111b30" : "#eff3fb";
}

function displayName(name) {
  return name === state.name && state.preferences.displayName ? state.preferences.displayName : name;
}

function avatar(node, name, tone = name === state.name ? state.preferences.avatarTone : "auto") {
  node.setAttribute("aria-hidden", "true");
  if (!name) {
    node.style.removeProperty("--hue");
    node.replaceChildren(icon("user"));
    return node;
  }
  const parts = name.split(/[_-]+/).filter(Boolean);
  const initials = parts.length > 1 ? parts[0][0] + parts[1][0] : (parts[0] || name).slice(0, 2);
  node.textContent = initials.toUpperCase();
  const index = [...name].reduce((sum, letter) => (sum * 31 + letter.charCodeAt(0)) >>> 0, 0);
  node.style.setProperty("--hue", tone === "auto" ? avatarPalette[index % avatarPalette.length] : tone);
  return node;
}

async function api(path, data) {
  const options = data === undefined
    ? {cache: "no-store"}
    : {method: "POST", headers: {"Content-Type": "application/json"}, body: JSON.stringify(data)};
  const response = await fetch("/api/" + path, options);
  const result = await response.json();
  if (!response.ok) {
    const error = new Error(result.error || "请求失败");
    error.status = response.status;
    throw error;
  }
  return result;
}

function note(message = "", error = false) {
  $("composer-note").textContent = message;
  $("composer-note").classList.toggle("error", error);
}

function setIdentity(name, snapshot = {}) {
  clearInterval(state.refresh);
  state.epoch++;
  state.name = name;
  state.preferences = readPreferences(name);
  applyAppearance();
  if (!name) $("settings").close();
  state.cursor = snapshot.cursor || 0;
  state.group = [];
  state.private.clear();
  state.unread.clear();
  state.history = null;
  state.members = [];
  state.view = "group";
  state.peer = null;
  state.sendBusy = false;
  state.uploadBusy = false;

  $("welcome").hidden = !!name;
  $("workspace").hidden = !name;
  $("messages").hidden = !name;
  $("logout").hidden = !name;
  $("identity-name").textContent = displayName(name) || "访客";
  $("identity-status").textContent = name ? "在线" : "尚未连接";
  $("connection").classList.toggle("online", !!name);
  $("connection").classList.remove("reconnecting");
  $("connection-label").textContent = name ? "已连接" : "等待连接";
  $("draft").value = "";
  avatar($("identity-avatar"), name);
  renderMembers();
  choose("group");
  if (name) {
    for (const event of snapshot.private || []) onEvent(event, true);
    receive(state.epoch);
    loadHistory();
    roster();
    state.refresh = setInterval(roster, 3500);
  }
}

async function roster() {
  if (state.name) {
    try {
      await api("chat", {action: "who"});
    } catch (error) {
      if (error.status === 401) return;
      console.debug(error.message);
    }
  }
}

function renderMembers() {
  $("people-count").textContent = state.name ? state.members.length : "—";
  $("members-note").hidden = !!state.members.length;
  $("members-note").textContent = state.name ? "正在读取在线成员…" : "连接后可查看在线成员，点击成员开始私聊。";
  $("members").replaceChildren(...state.members.map((name) => {
    const row = el("li");
    const button = el("button", "member");
    button.type = "button";
    button.append(avatar(el("span", "avatar"), name), el("strong", "", displayName(name)));
    if (name === state.name) {
      button.append(el("small", "", "我"));
      button.disabled = true;
    } else {
      button.append(el("small", "dot"));
      button.setAttribute("aria-label", "私聊 " + name);
      button.addEventListener("click", () => choose("private", name, true));
    }
    row.append(button);
    return row;
  }));
}

function renderNavigation() {
  for (const [view, id] of Object.entries({group: "nav-channel", history: "nav-history", files: "nav-files"})) {
    $(id).classList.toggle("selected", state.view === view);
    if (state.view === view) $(id).setAttribute("aria-current", "page");
    else $(id).removeAttribute("aria-current");
  }
  $("private-nav").hidden = !state.private.size;
  $("conversations").replaceChildren(...Array.from(state.private.keys(), (name) => {
    const active = state.view === "private" && state.peer === name;
    const button = el("button", "nav-item" + (active ? " selected" : ""));
    button.type = "button";
    if (active) button.setAttribute("aria-current", "page");
    const count = state.unread.get(name);
    button.append(avatar(el("span", "avatar"), name), el("span", "conversation-name", name));
    if (count) {
      button.append(el("span", "unread", String(count)));
      button.setAttribute("aria-label", name + "，" + count + " 条未读消息");
    }
    button.addEventListener("click", () => choose("private", name, true));
    return button;
  }));
}

function choose(view, peer = null, focus = false) {
  if (!state.name && view !== "group") {
    $("username").focus();
    return;
  }
  if (peer && !state.private.has(peer)) state.private.set(peer, []);
  if (peer) state.unread.delete(peer);
  state.view = view;
  state.peer = peer;
  const [title, description, glyph] = peer ? [peer, "私聊", "people"] : panels[view];
  $("channel-name").textContent = title;
  $("channel-description").textContent = description;
  $("channel-icon").replaceChildren(icon(glyph));
  $("composer-area").hidden = !state.name || view === "history" || view === "files";
  $("attach").hidden = view !== "group";
  $("attach").disabled = state.uploadBusy;
  $("send").disabled = state.sendBusy;
  $("draft").placeholder = peer ? "发送私聊给 " + peer : "发送消息到公共聊天室";
  if (!state.uploadBusy) note();
  renderNavigation();
  renderMessages(true);
  if (view === "history" || view === "files") loadHistory();
  if (focus) $("draft").focus();
}

function record(text) {
  const head = text.match(/^\[(.*?)\] \[(群聊|私聊|群文件)\] (.*)$/);
  if (!head) return null;
  const [, date, kind, body] = head;
  const base = {date, kind, key: text};
  if (kind === "群文件") {
    const file = body.match(/^(\S+) 上传 (.+) \((\d+) 字节\)，ID ([a-f0-9]{32})$/);
    return file ? {...base, sender: file[1], file: file[2], size: Number(file[3]), id: file[4]} : null;
  }
  if (kind === "私聊") {
    const direct = body.match(/^(\S+) → (\S+): (.+)$/);
    return direct ? {...base, sender: direct[1], recipient: direct[2], text: direct[3]} : null;
  }
  const group = body.match(/^(\S+): (.+)$/);
  return group ? {...base, sender: group[1], text: group[2]} : null;
}

function sizeLabel(bytes) {
  return bytes < 1024 ? bytes + " B" : bytes < 1048576
    ? (bytes / 1024).toFixed(1) + " KB" : (bytes / 1048576).toFixed(1) + " MB";
}

function messageCard(message, isNew) {
  const mine = message.sender === state.name;
  const row = el("article", "message" + (mine ? " mine" : ""));
  if (isNew) row.classList.add("is-new");
  const content = el("div", "message-content");
  const meta = el("header", "message-meta");
  const time = el("time", "", message.date.slice(11, 16));
  time.dateTime = message.date.replace(" ", "T");
  time.title = message.date;
  meta.append(el("b", "", mine ? displayName(message.sender) + " · 我" : message.sender), time);
  content.append(meta);
  if (message.file) {
    const card = el("div", "file-card");
    const detail = el("div");
    const download = el("a", "icon-action");
    download.href = "/api/files/" + message.id;
    download.download = message.file;
    download.setAttribute("aria-label", "下载 " + message.file);
    download.title = "下载文件";
    download.append(icon("download"));
    detail.append(el("b", "", message.file), el("small", "", sizeLabel(message.size) + " · 群文件"));
    card.append(icon("file"), detail, download);
    content.append(card);
  } else {
    content.append(el("div", "bubble", message.text));
  }
  row.append(avatar(el("span", "avatar"), message.sender), content);
  return row;
}

function renderMessages(forceBottom = false, arrivals) {
  const panel = $("messages");
  const follow = forceBottom || panel.scrollHeight - panel.scrollTop - panel.clientHeight < 105;
  let list = state.view === "private" ? state.private.get(state.peer) || [] : state.group;
  if (state.view === "history") list = list.slice(-20);
  if (state.view === "files") list = list.filter((message) => message.file);
  const [title, detail] = ({
    group: ["还没有消息", "发一条消息开始聊天。"],
    private: ["还没有私聊消息", "这里的消息只发给对话参与者。"],
    history: ["暂无历史消息", "这里显示服务端最近保存的公共记录。"],
    files: ["暂无群文件", "在公共聊天室点击附件按钮上传文件。"]
  })[state.view];
  $("empty-chat").hidden = !!list.length;
  $("empty-title").textContent = title;
  $("empty-detail").textContent = detail;

  let previousDay;
  const cards = [];
  for (const message of list) {
    const day = message.date.slice(0, 10);
    if (day !== previousDay) {
      const divider = el("div", "day-divider");
      divider.append(icon("calendar"), el("time", "", day));
      divider.lastChild.dateTime = day;
      cards.push(divider);
      previousDay = day;
    }
    cards.push(messageCard(message, arrivals?.has(message)));
  }
  $("message-list").replaceChildren(...cards);
  if (follow) panel.scrollTop = panel.scrollHeight;
}

async function loadHistory() {
  if (state.history || !state.name) return;
  const snapshot = {messages: []};
  state.history = snapshot;
  try {
    await api("chat", {action: "history"});
  } catch (error) {
    if (state.history === snapshot) {
      state.history = null;
      note(error.message, true);
    }
  }
}

function onEvent(event, restored = false) {
  if (event.kind === 22) {
    if (state.history) state.history = null;
    note(event.text, true);
    return false;
  }
  if (event.kind === 23 && state.history && event.text === "历史查询结束") {
    const recent = state.history.messages;
    // Keep multiplicity when identical messages were sent in the same second.
    const counts = new Map();
    for (const item of recent) counts.set(item.key, (counts.get(item.key) || 0) + 1);
    const previous = state.group.filter((item) => {
      const count = counts.get(item.key) || 0;
      if (count) counts.set(item.key, count - 1);
      return !count;
    });
    state.group = previous.concat(recent).slice(-250);
    state.history = null;
    return true;
  }
  if (event.kind !== 21) return false;
  if (/^在线用户(?: |$)/.test(event.text)) {
    const names = event.text.split(" ").slice(1);
    if (names.join(" ") !== state.members.join(" ")) {
      state.members = names;
      renderMembers();
    }
    return false;
  }
  const message = record(event.text);
  if (!message) return false;
  if (message.recipient) {
    const peer = message.sender === state.name ? message.recipient : message.sender;
    const list = state.private.get(peer) || [];
    state.private.set(peer, list.concat(message).slice(-150));
    const active = state.view === "private" && state.peer === peer;
    if (!restored && !active && message.sender !== state.name) state.unread.set(peer, (state.unread.get(peer) || 0) + 1);
    renderNavigation();
    return active ? message : false;
  }
  if (state.history) {
    state.history.messages.push(message);
    return false;
  }
  state.group = state.group.concat(message).slice(-250);
  return message;
}

async function receive(epoch) {
  while (epoch === state.epoch && state.name) {
    try {
      const page = await api("events?after=" + state.cursor);
      if (epoch !== state.epoch) return;
      let changed = false;
      const arrivals = new Set();
      for (const event of page.events) {
        state.cursor = event.id;
        const result = onEvent(event);
        if (result) changed = true;
        if (result && result !== true) arrivals.add(result);
      }
      if (changed) renderMessages(false, arrivals);
      if (page.closed) throw new Error("服务端连接已关闭，请重新登录。");
      $("connection-label").textContent = "已连接";
      $("connection").classList.remove("reconnecting");
      $("connection").classList.add("online");
    } catch (error) {
      if (epoch !== state.epoch) return;
      if (error.status === 401 || error.message.startsWith("服务端连接")) {
        setIdentity(null);
        authMessage("连接已断开，请重新登录。");
        return;
      }
      $("connection-label").textContent = "正在重连";
      $("connection").classList.remove("online");
      $("connection").classList.add("reconnecting");
      await new Promise((resolve) => setTimeout(resolve, 1800));
    }
  }
}

let registering = false;
let authBusy = false;

function authMessage(text = "", success = false) {
  const message = $("login-error");
  message.textContent = text;
  message.classList.toggle("success", success);
}

function showPassword(visible) {
  $("password").type = visible ? "text" : "password";
  $("toggle-password").textContent = visible ? "隐藏" : "显示";
  $("toggle-password").setAttribute("aria-pressed", String(visible));
}

function setAuthMode(register) {
  registering = register;
  $("auth-login").setAttribute("aria-pressed", String(!register));
  $("auth-register").setAttribute("aria-pressed", String(register));
  $("auth-title").textContent = register ? "创建账号" : "登录聊天室";
  $("auth-intro").textContent = register ? "使用自己的邮箱创建 Linkroom 账号。" : "使用你的 Linkroom 账号继续。";
  $("auth-action").textContent = register ? "创建账号" : "进入聊天室";
  $("email-field").hidden = !register;
  $("email").disabled = !register;
  if (!register) $("email").value = "";
  $("password").value = "";
  $("password").placeholder = register ? "设置 8–64 个字符的密码" : "输入密码";
  $("password").autocomplete = register ? "new-password" : "current-password";
  showPassword(false);
  authMessage();
}

for (const [id, register] of [["auth-login", false], ["auth-register", true]]) {
  $(id).addEventListener("click", () => {
    if (register !== registering) setAuthMode(register);
  });
}

$("toggle-password").addEventListener("click", () => {
  showPassword($("password").type === "password");
});

$("login-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  if (authBusy) return;
  authMessage();
  const name = $("username").value.trim();
  const password = $("password").value;
  const length = [...password].length;
  if (registering && (length < 8 || length > 64)) {
    authMessage("密码须为 8–64 个字符。");
    $("password").focus();
    return;
  }

  authBusy = true;
  for (const id of ["join", "auth-login", "auth-register"]) $(id).disabled = true;
  try {
    if (registering) {
      await api("register", {name, email: $("email").value.trim(), password});
      setAuthMode(false);
      authMessage("账号已创建，请输入密码登录。", true);
      $("password").focus();
    } else {
      const result = await api("session", {name, password});
      $("password").value = "";
      showPassword(false);
      setIdentity(result.name, result);
      $("draft").focus();
    }
  } catch (error) {
    authMessage(error.message);
  } finally {
    authBusy = false;
    for (const id of ["join", "auth-login", "auth-register"]) $(id).disabled = false;
  }
});

$("logout").addEventListener("click", async () => {
  try {
    await api("logout", {});
  } catch (error) {
    if (error.status !== 401) {
      note(error.message, true);
      return;
    }
  }
  setIdentity(null);
  $("username").focus();
});

const settings = $("settings");
const settingsForm = $("settings-form");

function previewProfile() {
  $("settings-name").textContent = $("display-name").value.trim() || state.name;
  avatar($("settings-avatar"), state.name, settingsForm.elements.namedItem("avatarTone").value);
}

$("open-settings").addEventListener("click", () => {
  if (!state.name) return;
  const preferences = state.preferences;
  settingsForm.elements.namedItem("avatarTone").value = preferences.avatarTone;
  $("display-name").value = preferences.displayName;
  $("display-name").placeholder = state.name;
  $("settings-account").textContent = "用户名 · " + state.name;
  $("theme").value = preferences.theme;
  $("reduce-motion").checked = preferences.reduceMotion;
  $("enter-sends").checked = preferences.enterSends;
  $("settings-feedback").hidden = true;
  previewProfile();
  settings.showModal();
});

settingsForm.addEventListener("input", (event) => {
  if (event.target.name === "avatarTone" || event.target.id === "display-name") previewProfile();
});
for (const id of ["close-settings", "cancel-settings"]) {
  $(id).addEventListener("click", () => settings.close());
}
settingsForm.addEventListener("submit", (event) => {
  event.preventDefault();
  if (!state.name) return;
  const preferences = {
    displayName: $("display-name").value.trim(),
    avatarTone: settingsForm.elements.namedItem("avatarTone").value,
    theme: $("theme").value,
    reduceMotion: $("reduce-motion").checked,
    enterSends: $("enter-sends").checked
  };
  try {
    localStorage.setItem(settingsKey(state.name), JSON.stringify(preferences));
  } catch {
    $("settings-feedback").textContent = "当前浏览器无法保存设置。";
    $("settings-feedback").hidden = false;
    return;
  }
  state.preferences = preferences;
  applyAppearance();
  $("identity-name").textContent = displayName(state.name);
  avatar($("identity-avatar"), state.name);
  renderMembers();
  renderMessages();
  settings.close();
});

systemDark.addEventListener("change", applyAppearance);
applyAppearance();

$("nav-channel").addEventListener("click", () => choose("group"));
$("nav-history").addEventListener("click", () => choose("history"));
$("nav-files").addEventListener("click", () => choose("files"));
$("draft").addEventListener("keydown", (event) => {
  if (state.preferences.enterSends && event.key === "Enter" && !event.isComposing && !event.shiftKey) {
    event.preventDefault();
    $("composer").requestSubmit();
  }
});

$("composer").addEventListener("submit", async (event) => {
  event.preventDefault();
  if (state.sendBusy || !state.name) return;
  const original = $("draft").value;
  const text = original.trim().replaceAll(/\r?\n/g, " ");
  if (!text) return;
  const user = state.peer;
  const bytes = new TextEncoder().encode(user ? user + " " + text : text).length;
  if (bytes > 1024) {
    note("消息太长，上限为 1024 字节。", true);
    return;
  }
  const epoch = state.epoch;
  state.sendBusy = true;
  $("send").disabled = true;
  try {
    await api("chat", {action: user ? "private" : "group", text, user});
    if (epoch !== state.epoch) return;
    if ($("draft").value === original) $("draft").value = "";
    if (!state.uploadBusy) note();
  } catch (error) {
    if (epoch === state.epoch) note(error.message, true);
  } finally {
    if (epoch === state.epoch) {
      state.sendBusy = false;
      $("send").disabled = false;
      $("draft").focus();
    }
  }
});

$("attach").addEventListener("click", () => $("file-picker").click());
$("file-picker").addEventListener("change", () => {
  const file = $("file-picker").files[0];
  $("file-picker").value = "";
  if (!file || state.uploadBusy || !state.name) return;
  if (file.size > 1024 ** 3) {
    note("文件上限为 1 GiB。", true);
    return;
  }
  const epoch = state.epoch;
  state.uploadBusy = true;
  $("attach").disabled = true;
  note("正在上传 " + file.name);
  const request = new XMLHttpRequest();
  request.open("POST", "/api/upload?name=" + encodeURIComponent(file.name));
  request.responseType = "json";
  request.upload.addEventListener("progress", (event) => {
    if (epoch !== state.epoch) return;
    const progress = event.lengthComputable && event.total ? Math.round(event.loaded * 100 / event.total) : 0;
    note(progress === 100 ? "上传完毕，服务端正在校验…" : "正在上传 " + file.name + " · " + progress + "%");
  });
  const finish = (message, error = false) => {
    if (epoch !== state.epoch) return;
    state.uploadBusy = false;
    $("attach").disabled = false;
    note(message, error);
  };
  request.addEventListener("load", () => {
    if (request.status === 200) finish("文件已共享：" + file.name);
    else finish(request.response?.error || "上传失败。", true);
  });
  request.addEventListener("error", () => finish("上传连接已断开。", true));
  request.send(file);
});

api("session").then((snapshot) => {
  if (snapshot.name && !state.name) setIdentity(snapshot.name, snapshot);
}).catch(() => {
  authMessage("网页网关无法连接，请确认它仍在运行。");
});
