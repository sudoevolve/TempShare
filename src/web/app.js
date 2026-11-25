'use strict';

import { gather, peerConnection, timedFetch } from './rtc.js';
import { fingerprint, forgetUpload, rememberUpload, resumableUploads, savedHandle } from './resume.js';

const byId = (id) => document.getElementById(id);
const pairForm = byId('pair-form');
const dropzone = byId('dropzone');
const fileInput = byId('file-input');
const queue = [];
const maxFileSize = 1024 ** 3;
const chunkSize = 4 * 1024 ** 2;
const pollInterval = 1100;
let connection = '';
let computerName = '';
let sessionSeconds = 0;
let generation = 0;
let sharedEntries = [];
let downloadAllowed = true;
let folderPath = [];
let folderEntries = [];
let folderRequest = 0;
let folderLoading = false;
let folderError = '';
const folderPageSize = 80;
let folderTotal = 0;
let folderSearchTimer;
let selectingFolder = false;
const selectedFolderEntries = new Set();
let shareRestored = false;
let polling = false;
let pairing = false;
let pairingAttempt = 0;
let interrupted = false;
let tab = 'share';
let shownMessages = '';
let syncMessages = [];
let unreadMessages = 0;
let knownTextRevision = -1;
let knownFilesRevision = -1;
let knownOffersRevision = -1;
let knownOffers = [];
let watching = false;
let castPeer;
let castRevision = 0;
let castGeneration = 0;
let castViewer = '';
let refreshingCast = false;
let captureReady = false;
let screenLock;
let castStarted = 0;
let castAnsweredAt = 0;
let lastVideoTime = -1;
let lastFrameAt = 0;
let lastVideoFrames = 0;
let useFrames = false;
let frameSequence = '0';
let frameTimer;
let framesInWindow = 0;
let frameWindowAt = 0;
const castVideo = byId('cast-video');
const castCanvas = byId('cast-frames');
let shownRecovery = '';
const trustedConnect = byId('trusted-connect');
trustedConnect.hidden = location.protocol !== 'https:';
byId('resume-option').hidden = location.protocol !== 'https:' || !crypto.subtle;

function message(text, error = false) {
  const notice = byId('message');
  notice.textContent = text;
  notice.hidden = !text;
  notice.classList.toggle('error', error);
}

function showConnection(state, verification = '', reason = '') {
  const states = {
    '': ['未连接', '连接到电脑', '输入电脑上显示的连接码。'],
    pending: ['待确认', '等待电脑确认', '请在电脑上核对以下验证码并允许连接。'],
    approved: ['已连接', '', ''],
  };
  const [badge, title, description] = states[state];
  const changed = connection !== state;
  connection = state;
  byId('connection').dataset.state = state;
  const connected = state === 'approved';
  byId('connection').textContent = connected && computerName ? `${computerName} · ${badge}` : badge;
  document.body.classList.toggle('paired', connected);
  const disconnect = byId('disconnect');
  disconnect.hidden = state === '';
  disconnect.textContent = state === 'pending' ? '取消请求' : '断开';
  byId('retry-connection').hidden = state !== '' || pairing;
  byId('connect-title').textContent = title;
  byId('connect-description').textContent = state === 'pending' && sessionSeconds
    ? `${description} 剩余 ${sessionSeconds} 秒。` : description;
  byId('connect-panel').hidden = state === 'approved';
  byId('feature-nav').hidden = state !== 'approved';
  for (const feature of ['share', 'sync', 'cast']) {
    byId(feature + '-panel').hidden = state !== 'approved' || tab !== feature;
  }
  pairForm.hidden = state !== '';
  byId('verification').hidden = state !== 'pending';
  byId('verification').textContent = verification;
  if (changed && state === '') {
    generation++;
    for (const item of queue) item.request?.abort();
    queue.length = 0;
    updateQueueSummary();
    if (byId('file-preview').open) byId('file-preview').close();
    byId('upload-list').replaceChildren();
    byId('uploads').hidden = true;
    showDownloads([]);
    shareRestored = false;
    showMessages([]);
    knownTextRevision = -1;
    knownFilesRevision = -1;
    knownOffersRevision = -1;
    knownOffers = [];
    forgetUpload();
    showRecovery([]);
    byId('sync-text').value = '';
    showCastAvailability(false);
    stopViewing(false);
    const reasons = {
      pairing_denied: '电脑拒绝了连接。请扫码或输入新连接码。',
      approval_expired: '等待确认超时。请使用电脑上的新连接码。',
      session_expired: '会话已到期。请在电脑上新建会话，再扫码或输入连接码。',
      session_ended: '连接已结束。请使用电脑上显示的新连接码。',
    };
    message(reasons[reason] || reasons.session_ended, true);
  } else if (changed) {
    message('');
  }
}

function request(path, method = 'GET', headers = {}, body) {
  return timedFetch(path, { method, headers: method === 'GET' ? headers : { 'X-TempShare': '1', ...headers },
    body, cache: 'no-store' });
}

function showTab(feature) {
  if (tab === feature) return;
  tab = feature;
  if (tab === 'sync') {
    unreadMessages = 0;
    byId('sync-unread').hidden = true;
    byId('sync-alert').hidden = true;
  }
  for (const button of byId('feature-nav').querySelectorAll('[data-panel]')) {
    const selected = button.dataset.panel === tab;
    button.setAttribute('aria-pressed', String(selected));
    byId(button.dataset.panel + '-panel').hidden = !selected;
  }
  byId('resume-watching').hidden = !watching || tab === 'cast';
  poll();
}

async function askToPair(invitation) {
  if (pairing || connection) return;
  pairing = true;
  pairingAttempt++;
  pairForm.querySelector('button').disabled = true;
  try {
    const device = byId('device-name').value.trim() ||
      navigator.userAgent.match(/iPhone|iPad|Android|Windows|Macintosh|Linux/)?.[0] || '浏览器';
    const response = await request('/api/pair', 'POST', {
      'X-Invitation': invitation,
      'X-Device-Name': encodeURIComponent(device),
    });
    if (!response.ok) {
      message(response.status === 403
        ? '电脑拒绝了此连接。请查看桌面端的连接码并核对当前局域网地址。'
        : '连接码已过期或不正确，请核对电脑上显示的连接码。', true);
      return;
    }
    const data = await response.json();
    showConnection('pending', data.verification);
    byId('pair-code').value = '';
  } catch {
    message(`无法连接到 ${location.host}。请连接电脑所在的家庭或办公网络，关闭访客隔离后重试。`, true);
  } finally {
    pairing = false;
    pairForm.querySelector('button').disabled = false;
  }
}

function formatSize(bytes) {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 ** 2) return `${(bytes / 1024).toFixed(1)} KiB`;
  if (bytes < 1024 ** 3) return `${(bytes / 1024 ** 2).toFixed(1)} MiB`;
  return `${(bytes / 1024 ** 3).toFixed(1)} GiB`;
}

function element(tag, className, value) {
  const node = document.createElement(tag);
  node.className = className;
  if (value !== undefined) node.textContent = value;
  return node;
}

function fileRow(name, subtitle) {
  const row = element('li', 'file-row');
  const details = element('div', 'file-details');
  const meta = element('p', 'file-meta', subtitle);
  details.append(element('p', 'file-name', name), meta);
  row.append(details);
  return { row, meta };
}

function showDownloads(files) {
  sharedEntries = files;
  const available = (path) => path.length && files.some((entry) => entry.folder && entry.id === path[0].id);
  if (!shareRestored && files.length) {
    shareRestored = true;
    const stored = history.state?.folder || [];
    if (Array.isArray(stored) && available(stored)) {
      openFolder(stored);
      return;
    }
  }
  if (available(folderPath)) {
    openFolder(folderPath, false, false, true);
    return;
  }
  openFolder([]);
}

async function previewFile(file, href) {
  const viewer = byId('file-preview');
  const content = byId('preview-content');
  const target = new URL(href, location.href);
  target.searchParams.set('preview', '1');
  byId('preview-name').textContent = file.name;
  const status = element('p', 'empty', '正在加载…');
  status.setAttribute('role', 'status');
  content.replaceChildren(status);
  viewer.showModal();
  try {
    const response = await request(target.href, 'HEAD');
    if (!response.ok) throw new Error('preview');
    if (!viewer.open || !content.contains(status)) return;
    if (file.preview.startsWith('text/')) {
      const result = await request(target.href);
      if (!result.ok) throw new Error('preview');
      const value = await result.text();
      if (content.contains(status)) content.replaceChildren(element('pre', 'sync-body', value));
      return;
    }
    if (file.preview.startsWith('image/')) {
      const image = element('img', 'preview-media');
      image.alt = file.name;
      image.src = target.href;
      image.addEventListener('error', () => image.replaceWith(element('p', 'empty', '无法打开图片。')));
      status.replaceWith(image);
    } else if (file.preview.startsWith('video/')) {
      const video = element('video', 'preview-media');
      video.src = target.href;
      video.controls = true;
      video.playsInline = true;
      video.addEventListener('error', () => video.replaceWith(element('p', 'empty', '当前浏览器无法播放此视频。')));
      status.replaceWith(video);
    } else {
      const pdf = element('a', 'download', '在新标签页打开 PDF');
      pdf.href = target.href;
      pdf.target = '_blank';
      pdf.rel = 'noopener noreferrer';
      status.replaceWith(pdf);
    }
  } catch {
    if (content.contains(status)) status.textContent = '无法预览，文件可能已移除或发生变化。';
  }
}

byId('close-preview').addEventListener('click', () => byId('file-preview').close());
byId('file-preview').addEventListener('close', () => byId('preview-content').replaceChildren());

function downloadRow(file, root) {
  let description = formatSize(file.size);
  if (file.folder) description = '文件夹';
  if (file.unavailable) description = file.size > maxFileSize ? '超过单文件 1 GiB 上限' : '文件无法读取';
  const { row } = fileRow(file.name, description);
  row.dataset.entry = file.id;
  if (file.folder) {
    const open = element('button', 'folder-open', file.name);
    open.type = 'button';
    open.setAttribute('aria-label', `打开文件夹 ${file.name}`);
    row.querySelector('.file-name').replaceChildren(open);
    row.classList.add('folder-row');
    open.addEventListener('click', () => openFolder([...folderPath, file], true, true));
    if (selectingFolder && root) row.append(folderCheckbox(file));
    return row;
  }
  if (file.unavailable) return row;
  const link = element('a', 'download', '下载');
  link.href = root ? `/api/folders/${encodeURIComponent(root.id)}/download/${encodeURIComponent(file.id)}` :
    `/api/download/${encodeURIComponent(file.id)}`;
  link.download = file.name;
  link.setAttribute('aria-label', `下载 ${file.name}`);
  const error = element('p', 'download-error', '');
  error.setAttribute('role', 'status');
  error.hidden = true;
  row.querySelector('.file-details').append(error);
  link.addEventListener('click', async (event) => {
    event.preventDefault();
    if (link.getAttribute('aria-busy') === 'true') return;
    link.setAttribute('aria-busy', 'true');
    error.hidden = true;
    try {
      const response = await request(link.href, 'HEAD');
      if (!response.ok) {
        const details = {
          403: '请重新配对后重试。',
          404: '文件已移除或下载权限已关闭。',
          409: '源文件已变动，请让电脑刷新目录。',
          503: '电脑正在传输其他文件，稍后重试。',
        };
        error.textContent = details[response.status] || '暂时无法下载，请重试。';
        error.hidden = false;
        return;
      }
      const nativeDownload = link.cloneNode();
      nativeDownload.hidden = true;
      row.append(nativeDownload);
      nativeDownload.click();
      nativeDownload.remove();
    } catch {
      error.textContent = '与电脑的连接中断，请重试。';
      error.hidden = false;
    } finally { link.removeAttribute('aria-busy'); }
  });
  const actions = element('div', 'file-action');
  if (file.preview && typeof byId('file-preview').showModal === 'function') {
    const preview = element('button', '', '预览');
    preview.type = 'button';
    preview.setAttribute('aria-label', `预览 ${file.name}`);
    preview.addEventListener('click', () => previewFile(file, link.href));
    actions.append(preview);
  }
  actions.append(link);
  if (selectingFolder && root) actions.prepend(folderCheckbox(file));
  row.append(actions);
  return row;
}

function folderCheckbox(file) {
  const checkbox = element('input', 'folder-checkbox');
  checkbox.type = 'checkbox';
  checkbox.checked = selectedFolderEntries.has(file.id);
  checkbox.setAttribute('aria-label', `选择 ${file.name}`);
  checkbox.disabled = !checkbox.checked && selectedFolderEntries.size >= 80;
  checkbox.addEventListener('change', () => {
    if (checkbox.checked) selectedFolderEntries.add(file.id);
    else selectedFolderEntries.delete(file.id);
    for (const item of byId('download-list').querySelectorAll('.folder-checkbox')) {
      item.disabled = !item.checked && selectedFolderEntries.size >= 80;
    }
    updateZipLink();
  });
  return checkbox;
}

function updateZipLink() {
  const link = byId('folder-zip');
  const root = folderPath[0];
  const current = folderPath[folderPath.length - 1];
  link.hidden = !current || (selectingFolder && !selectedFolderEntries.size);
  if (link.hidden) return;
  const href = new URL(`/api/folders/${root.id}/zip/${current.id}`, location.href);
  if (selectingFolder) for (const id of selectedFolderEntries) href.searchParams.append('file', id);
  link.href = href.href;
  link.textContent = selectingFolder ? `下载所选 ${selectedFolderEntries.size} 项 ZIP` : '整目录 ZIP';
}

function renderFolder() {
  const root = folderPath[0];
  const current = folderPath[folderPath.length - 1];
  const list = byId('download-list');
  byId('folder-toolbar').hidden = !current;
  byId('folder-filter').hidden = !current;
  byId('folder-actions').hidden = !current || folderLoading || !!folderError;
  byId('folder-select').setAttribute('aria-pressed', String(selectingFolder));
  byId('folder-select').textContent = selectingFolder ? '取消多选' : '多选';
  updateZipLink();
  const crumbs = [element('button', '', '共享')];
  crumbs[0].type = 'button';
  crumbs[0].addEventListener('click', () => openFolder([], true, true));
  folderPath.forEach(({ name }, index) => {
    if (index === folderPath.length - 1) {
      const active = element('span', '', name);
      active.setAttribute('aria-current', 'location');
      crumbs.push(active);
    } else {
      const link = element('button', '', name);
      link.type = 'button';
      link.addEventListener('click', () => openFolder(folderPath.slice(0, index + 1), true, true));
      crumbs.push(link);
    }
  });
  byId('folder-path').replaceChildren(...crumbs);
  const entries = root ? folderEntries : sharedEntries;
  const focus = list.contains(document.activeElement) ? document.activeElement.closest('[data-entry]')?.dataset.entry : '';
  list.replaceChildren(...entries.map((entry) => downloadRow(entry, root)));
  if (focus) [...list.children].find((row) => row.dataset.entry === focus)?.querySelector('button, a')?.focus({ preventScroll: true });
  byId('folder-more').hidden = !root || entries.length >= folderTotal || !!folderError;
  byId('folder-more').disabled = folderLoading;
  byId('folder-retry').hidden = !folderError;
  const empty = byId('no-files');
  empty.hidden = entries.length > 0;
  empty.textContent = !downloadAllowed ? '电脑关闭了文件下载权限。' : folderLoading ? '正在读取文件夹…' :
    folderError || (current ? (byId('folder-search').value.trim() ? '没有匹配的文件或文件夹' : '此文件夹为空') : '暂无共享内容');
}

async function openFolder(path, focus = false, push = false, refresh = false, append = false) {
  clearTimeout(folderSearchTimer);
  const revision = ++folderRequest;
  folderPath = path;
  if (!refresh) {
    folderEntries = [];
    byId('folder-search').value = '';
    folderTotal = 0;
    selectingFolder = false;
    selectedFolderEntries.clear();
  }
  folderError = '';
  folderLoading = path.length > 0;
  const nextState = { ...history.state, folder: path.map(({ id, name }) => ({ id, name })) };
  if (push) history.pushState(nextState, '');
  else history.replaceState(nextState, '');
  if (!refresh || !path.length) renderFolder();
  if (focus) byId(path.length ? 'folder-back' : 'download-title').focus();
  if (!path.length) return;
  byId('folder-more').disabled = true;
  try {
    const root = encodeURIComponent(path[0].id);
    const directory = encodeURIComponent(path[path.length - 1].id);
    const offset = append ? folderEntries.length : 0;
    const params = new URLSearchParams({
      offset: String(offset),
      q: byId('folder-search').value.trim().slice(0, 100),
      sort: byId('folder-sort').value,
    });
    const response = await request(`/api/folders/${root}/entries/${directory}?${params}`);
    if (revision !== folderRequest) return;
    if (response.status === 404) {
      if (refresh) {
        openFolder(path.slice(0, -1), false, false, true);
        return;
      }
      folderError = '文件夹已移除或下载权限已关闭。';
      return;
    }
    if (!response.ok) throw new Error('folder contents');
    const entries = await response.json();
    if (revision === folderRequest) {
      folderEntries = append ? [...folderEntries, ...entries] : entries;
      folderTotal = Number(response.headers.get('X-Folder-Count') ?? folderEntries.length);
      if (!append && !params.get('q') && folderTotal <= folderPageSize) {
        const available = new Set(entries.map(({ id }) => id));
        for (const id of selectedFolderEntries) if (!available.has(id)) selectedFolderEntries.delete(id);
      }
    }
  } catch {
    if (revision === folderRequest) folderError = '无法读取文件夹，请重试。';
  } finally {
    if (revision === folderRequest) {
      folderLoading = false;
      renderFolder();
    }
  }
}

byId('folder-back').addEventListener('click', () => openFolder(folderPath.slice(0, -1), true, true));
byId('folder-retry').addEventListener('click', () => openFolder(folderPath, true));
window.addEventListener('popstate', (event) => {
  const folder = event.state?.folder || [];
  if (connection === 'approved') {
    showTab('share');
    openFolder(folder);
  }
});
byId('folder-search').addEventListener('input', () => {
  clearTimeout(folderSearchTimer);
  folderSearchTimer = setTimeout(() => openFolder(folderPath, false, false, true), 220);
});
byId('folder-sort').addEventListener('change', () => openFolder(folderPath, false, false, true));
byId('folder-select').addEventListener('click', () => {
  selectingFolder = !selectingFolder;
  selectedFolderEntries.clear();
  renderFolder();
});
byId('folder-more').addEventListener('click', () => {
  if (folderEntries.length < folderTotal) openFolder(folderPath, false, false, true, true);
});

async function copyText(value) {
  try {
    if (navigator.clipboard) {
      await navigator.clipboard.writeText(value);
    } else {
      const field = document.createElement('textarea');
      field.value = value;
      field.setAttribute('readonly', '');
      document.body.append(field);
      field.select();
      const copied = document.execCommand('copy');
      field.remove();
      if (!copied) throw new Error('copy');
    }
    message('已复制');
  } catch {
    message('浏览器无法访问剪贴板，请手动选中文本复制。', true);
  }
}

function showMessages(entries, refresh = false) {
  if (!refresh && tab !== 'sync') {
    const previous = new Set(syncMessages.map((entry) => JSON.stringify(entry)));
    unreadMessages += entries.filter((entry) => entry.desktop && !previous.has(JSON.stringify(entry))).length;
    if (!entries.length) unreadMessages = 0;
    byId('sync-unread').textContent = unreadMessages ? ` · ${unreadMessages}` : '';
    byId('sync-unread').hidden = !unreadMessages;
    byId('sync-alert').hidden = !unreadMessages;
    byId('sync-alert').textContent = `收到 ${unreadMessages} 条新消息 · 查看`;
  }
  syncMessages = entries;
  const query = byId('sync-search').value.trim().toLocaleLowerCase();
  const key = JSON.stringify([query, entries, sharedEntries.map(({ id }) => id)]);
  if (key === shownMessages) return;
  shownMessages = key;
  byId('sync-list').replaceChildren(...entries.filter((entry) => entry.text.toLocaleLowerCase().includes(query))
    .slice().reverse().map((entry) => {
    const row = element('li', 'file-row sync-entry');
    const content = element('div', 'file-details');
    const text = entry.text.trim();
    let url;
    try { if (!/\s/.test(text)) url = new URL(text); } catch { /* Plain text */ }
    const link = url && (url.protocol === 'https:' || url.protocol === 'http:');
    const body = element(link ? 'a' : 'pre', 'sync-body', entry.text);
    if (link) {
      body.href = url.href;
      body.target = '_blank';
      body.rel = 'noopener noreferrer';
    }
    const time = entry.sentAt ? ' · ' + new Date(entry.sentAt * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' }) : '';
    content.append(element('p', 'file-meta', (entry.desktop ? '电脑' : '本机') + time));
    if (entry.attachment) {
      const href = '/api/text/attachment/' + encodeURIComponent(entry.attachment);
      if (entry.attachmentType?.startsWith('image/')) {
        const image = element('img', 'sync-image');
        image.alt = entry.text;
        image.loading = 'lazy';
        image.src = href + '?preview=1';
        content.append(image);
      }
      const download = element('a', 'download', '下载 ' + entry.text);
      download.href = href;
      download.download = entry.text;
      content.append(download);
    } else {
      content.append(entry.attachment ? element('p', 'file-name', '图片 · ' + entry.text) : body);
    }
    const copy = element('button', '', '复制');
    copy.type = 'button';
    copy.addEventListener('click', () => copyText(entry.text));
    row.append(content, copy);
    return row;
  }));
}

byId('sync-search').addEventListener('input', () => showMessages(syncMessages, true));
byId('sync-alert').addEventListener('click', () => showTab('sync'));
byId('sync-latest').addEventListener('click', () => {
  byId('sync-search').value = '';
  showMessages(syncMessages, true);
  byId('sync-list').firstElementChild?.scrollIntoView({ block: 'nearest' });
});
byId('sync-export').addEventListener('click', () => {
  const lines = syncMessages.map((entry) => (entry.desktop ? '电脑' : '本机') + ' · ' + entry.text);
  const url = URL.createObjectURL(new Blob([lines.join('\n\n')], { type: 'text/plain;charset=utf-8' }));
  const link = element('a', '', '');
  link.href = url;
  link.download = 'TempShare-同步记录.txt';
  document.body.append(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 30000);
});
byId('sync-clear').addEventListener('click', async () => {
  if (!confirm('清空双方当前会话的同步记录？')) return;
  try {
    if (!(await request('/api/text', 'DELETE')).ok) throw new Error('clear');
    showMessages([]);
    knownTextRevision = -1;
    poll();
  } catch { message('无法清空同步记录，请重试。', true); }
});

async function sendText(event) {
  event.preventDefault();
  const input = byId('sync-text');
  const button = event.submitter;
  const value = input.value;
  const current = generation;
  if (!value.trim() || new TextEncoder().encode(value).length > 32768) {
    message('请输入有效文本，上限为 32 KiB。', true);
    return;
  }
  if (button) button.disabled = true;
  try {
    const response = await request('/api/text', 'POST', { 'Content-Type': 'text/plain; charset=utf-8' }, value);
    if (!response.ok) throw new Error('text');
    if (generation === current && input.value === value) input.value = '';
    poll();
  } catch {
    if (current === generation) message('文本发送失败，请确认连接和同步权限后重试。', true);
  } finally {
    if (button) button.disabled = false;
  }
}

function castStatus(text) {
  byId('cast-status').textContent = text;
}

function clearCastPicture() {
  castVideo.hidden = true;
  castCanvas.hidden = true;
  byId('cast-placeholder').hidden = false;
}

function showCastAvailability(ready) {
  captureReady = ready;
  byId('cast-ready').hidden = !ready;
  byId('cast-watch').disabled = !ready && !watching;
  if (!watching) castStatus(ready ? '电脑已选择共享来源。' : '等待电脑选择共享来源。');
}

function clearCastPeer() {
  const previous = castPeer;
  castPeer = undefined;
  previous?.close();
  castRevision = 0;
  castVideo.pause();
  castVideo.srcObject = null;
  castVideo.muted = true;
  byId('cast-audio').hidden = true;
  castVideo.tabIndex = -1;
  lastVideoTime = -1;
  lastVideoFrames = 0;
  castAnsweredAt = 0;
  castVideo.hidden = true;
}

function stopViewing(notify = true) {
  const wasWatching = watching;
  const viewer = castViewer;
  watching = false;
  updateScreenLock();
  castGeneration++;
  clearTimeout(frameTimer);
  frameTimer = undefined;
  useFrames = false;
  clearCastPeer();
  clearCastPicture();
  castCanvas.width = 1;
  castCanvas.height = 1;
  showCastAvailability(captureReady);
  byId('cast-watch').textContent = '开始观看';
  byId('cast-mode').hidden = true;
  byId('cast-fullscreen').hidden = true;
  byId('resume-watching').hidden = true;
  if (notify && wasWatching) fetch('/api/cast/viewer', {
    method: 'POST', headers: { 'X-TempShare': '1', 'X-Cast-Watch': '0', 'X-Cast-Viewer': viewer }, keepalive: true,
  }).catch(() => {});
}

async function receiveFrame(generation) {
  if (generation !== castGeneration || !useFrames) return;
  let delay = 70;
  try {
    const response = await request('/api/cast/frame', 'GET', {
      'X-Cast-Viewer': castViewer, 'X-Cast-After': frameSequence, 'X-Cast-Wait': '1',
    });
    if (response.status === 200) {
      delay = 0;
      const url = URL.createObjectURL(await response.blob());
      try {
        const image = new Image();
        image.src = url;
        await image.decode();
        if (generation !== castGeneration) return;
        if (castCanvas.width !== image.naturalWidth || castCanvas.height !== image.naturalHeight) {
          castCanvas.width = image.naturalWidth;
          castCanvas.height = image.naturalHeight;
        }
        castCanvas.getContext('2d').drawImage(image, 0, 0);
        frameSequence = response.headers.get('X-Cast-Sequence') || '0';
        lastFrameAt = Date.now();
        framesInWindow++;
        if (lastFrameAt - frameWindowAt >= 1000) {
          const fps = Math.round(framesInWindow * 1000 / (lastFrameAt - frameWindowAt));
          castStatus(`兼容播放 · ${castCanvas.width} × ${castCanvas.height} · ${fps} fps · 无音频`);
          framesInWindow = 0;
          frameWindowAt = lastFrameAt;
        }
        castCanvas.hidden = false;
        byId('cast-fullscreen').hidden = !document.fullscreenEnabled;
        byId('cast-placeholder').hidden = true;
      } finally {
        URL.revokeObjectURL(url);
      }
    }
  } catch {
    // 偶发丢帧由下一次请求恢复。
  } finally {
    if (generation === castGeneration && useFrames) {
      if (Date.now() - lastFrameAt > 3500) {
        clearCastPicture();
        castStatus('正在等待电脑送出画面…');
      }
      frameTimer = setTimeout(() => receiveFrame(generation), delay);
    }
  }
}

function checkCastPlayback() {
  if (!watching || useFrames) return;
  if (castVideo.readyState >= 2 && castVideo.videoWidth && castVideo.currentTime > Math.max(0, lastVideoTime)) {
    const quality = castVideo.getVideoPlaybackQuality?.();
    const frames = quality ? quality.totalVideoFrames - quality.droppedVideoFrames : 0;
    const now = Date.now();
    const fps = lastVideoTime >= 0 && lastFrameAt && now > lastFrameAt && frames > lastVideoFrames
      ? Math.round((frames - lastVideoFrames) * 1000 / (now - lastFrameAt)) : 0;
    lastVideoFrames = frames;
    lastVideoTime = castVideo.currentTime;
    lastFrameAt = now;
    castVideo.tabIndex = 0;
    byId('cast-fullscreen').hidden = !document.fullscreenEnabled && !castVideo.webkitEnterFullscreen;
    byId('cast-placeholder').hidden = true;
    castStatus(`正在播放 · ${castVideo.videoWidth} × ${castVideo.videoHeight}${fps ? ` · ${fps} fps` : ''}`);
  } else if (!castVideo.paused || lastVideoTime < 0) {
    const now = Date.now();
    const stalled = lastFrameAt ? now - lastFrameAt > 12000 :
      castAnsweredAt ? now - castAnsweredAt > 12000 : now - castStarted > 20000;
    if (stalled) {
      useFrames = true;
      byId('cast-mode').textContent = '重新尝试直连';
      clearCastPeer();
      clearCastPicture();
      castStatus('正在切换到兼容播放…');
    }
  }
}

async function refreshCast() {
  if (!watching || refreshingCast) return;
  const current = castGeneration;
  refreshingCast = true;
  try {
    checkCastPlayback();
    const response = await request('/api/cast/viewer', 'POST', {
      'X-Cast-Watch': '1', 'X-Cast-Viewer': castViewer, 'X-Cast-Frames': useFrames ? '1' : '0',
    });
    if (current !== castGeneration) return;
    if (response.status === 409) {
      stopViewing(false);
      castStatus('已有另一个标签页正在观看。');
      return;
    }
    if (!response.ok) throw new Error('viewer');
    const signal = await response.json();
    if (current !== castGeneration) return;
    if (signal.frames) {
      if (!frameTimer) {
        frameSequence = '0';
        lastFrameAt = Date.now();
        frameWindowAt = lastFrameAt;
        framesInWindow = 0;
        frameTimer = setTimeout(() => receiveFrame(current), 0);
      }
      return;
    }
    if (!signal.offer) {
      clearCastPeer();
      clearCastPicture();
      if (!signal.ready) {
        stopViewing(false);
        showCastAvailability(false);
      } else {
        castStatus('正在等待电脑发送直连候选，可以手动切换兼容播放。');
      }
      return;
    }
    if (signal.revision === castRevision) return;
    clearCastPeer();
    clearCastPicture();
    const peer = peerConnection();
    castPeer = peer;
    castRevision = signal.revision;
    const media = new MediaStream();
    peer.addEventListener('track', (event) => {
      if (peer !== castPeer) return;
      media.addTrack(event.track);
      if (event.track.kind === 'audio') {
        byId('cast-audio').hidden = false;
        byId('cast-audio').textContent = castVideo.muted ? '取消静音' : '静音';
      }
      if (event.track.kind !== 'video') return;
      castVideo.srcObject = media;
      castVideo.hidden = false;
      castVideo.play().catch(() => {
        if (peer === castPeer) castStatus('正在等待画面，稍后会尝试兼容播放。');
      });
      castStatus('已收到视频轨道，正在等待首帧…');
    });
    peer.addEventListener('connectionstatechange', () => {
      if (peer === castPeer && peer.connectionState === 'failed') castStatus('媒体连接中断，正在等待电脑重连。');
    });
    await peer.setRemoteDescription({ type: 'offer', sdp: signal.offer });
    const answer = await gather(peer, await peer.createAnswer());
    if (peer !== castPeer) return;
    const result = await request('/api/cast/answer', 'POST', {
      'X-Cast-Revision': String(signal.revision), 'X-Cast-Viewer': castViewer,
    }, answer);
    if (!result.ok) {
      const error = await result.json();
      if (peer === castPeer && error.code === 'no_lan_candidate') castStarted = Date.now() - 15000;
      throw new Error('answer');
    }
    if (peer === castPeer) castAnsweredAt = Date.now();
  } catch {
    if (current === castGeneration) {
      clearCastPeer();
      clearCastPicture();
      castStatus('正在等待可用的局域网媒体连接。');
    }
  } finally {
    refreshingCast = false;
  }
}

function updateQueueSummary() {
  const active = queue.filter((item) => !item.finished && !item.cancelled);
  const failed = active.filter((item) => item.failed).length;
  const finished = queue.filter((item) => item.finished && !item.cancelled).length;
  byId('queue-shortcut').hidden = !queue.length || connection !== 'approved';
  byId('queue-shortcut').textContent = `传输队列 · ${active.length}`;
  byId('upload-summary').textContent = `${active.length} 个待处理 · ${failed} 个失败 · ${finished} 个已结束`;
  byId('pause-uploads').disabled = !active.some((item) => item.approved && !item.failed);
  byId('pause-uploads').textContent = active.some((item) => item.approved && !item.failed && !item.paused)
    ? '暂停全部' : '继续全部';
  byId('retry-uploads').disabled = !failed;
  byId('clear-uploads').disabled = !queue.some((item) => item.finished || item.cancelled);
  const images = queue.filter((item) => item.image && !item.cancelled);
  const imageStatus = byId('sync-image-status');
  imageStatus.hidden = !images.length;
  if (images.length) imageStatus.textContent = `${images.filter((item) => item.finished).length} / ${images.length} 张图片已处理 · 详情见传输队列`;
}

byId('queue-shortcut').addEventListener('click', () => {
  showTab('share');
  byId('upload-title').focus();
});
byId('pause-uploads').addEventListener('click', () => {
  const active = queue.filter((item) => item.approved && !item.finished && !item.cancelled && !item.failed);
  const pause = active.some((item) => !item.paused);
  active.filter((item) => !!item.paused !== pause).forEach((item) => item.pause.click());
});
byId('retry-uploads').addEventListener('click', () => queue.filter((item) => item.failed).forEach((item) => item.retry.click()));
byId('clear-uploads').addEventListener('click', () => {
  for (let index = queue.length - 1; index >= 0; index--) {
    const item = queue[index];
    if (!item.finished && !item.cancelled) continue;
    item.row.remove();
    queue.splice(index, 1);
  }
  byId('uploads').hidden = !queue.length;
  updateQueueSummary();
});

function updateItem(item, status, progress, failed = false) {
  item.meta.textContent = `${formatSize(item.file.size)} · ${status}`;
  item.progress.hidden = progress === undefined;
  item.progress.value = progress ?? 0;
  item.retry.hidden = !failed;
  item.cancel.hidden = item.finished || item.cancelled;
  item.pause.hidden = item.finished || item.cancelled || !item.approved || item.failed;
  item.pause.textContent = item.paused ? '继续' : '暂停';
}

function submitFiles(files, image = false) {
  const batch = Array.from(crypto.getRandomValues(new Uint8Array(16)),
    (value) => value.toString(16).padStart(2, '0')).join('');
  Array.from(files).forEach((file) => submitFile(file, undefined, batch, image));
}

function submitFile(file, saved, batch = '', image = false, handle) {
  if (file.size > maxFileSize) {
    message(`${file.name} 超过 1 GiB，未加入队列。`, true);
    return;
  }
  if (queue.filter((item) => !item.finished && !item.cancelled).length >= 100) {
    message('本会话的文件队列已满。', true);
    return;
  }
  if (queue.length >= 200) {
    const index = queue.findIndex((item) => item.finished || item.cancelled);
    if (index >= 0) queue.splice(index, 1)[0].row.remove();
  }
  const { row, meta } = fileRow(file.name, '');
  const controls = element('div', 'file-action');
  const retry = element('button', '', '重试');
  const cancel = element('button', '', '取消');
  const pause = element('button', '', '暂停');
  const progress = document.createElement('progress');
  progress.max = 100;
  progress.setAttribute('aria-label', `${file.name} 发送进度`);
  controls.append(retry, pause, cancel);
  row.append(controls, progress);
  const directory = file.webkitRelativePath?.split('/').slice(0, -1).join('/') || '';
  const item = { file, row, meta, progress, retry, pause, cancel, generation, offset: 0, batch, directory, image,
    id: saved?.id, handle, attempts: 0, retryAt: 0, keep: byId('remember-uploads').checked && !byId('resume-option').hidden };
  queue.push(item);
  retry.addEventListener('click', () => {
    if (item.request || item.cancelled) return;
    if (item.id) {
      item.attempts = 0;
      item.retryAt = 0;
      item.failed = false;
      updateItem(item, '等待上传');
      knownOffersRevision = -1;
      poll();
    } else {
      sendOffer(item);
    }
  });
  pause.addEventListener('click', () => {
    item.paused = !item.paused;
    if (item.paused) item.request?.abort();
    updateItem(item, item.paused ? '已暂停' : '等待上传');
    if (!item.paused) poll();
  });
  cancel.addEventListener('click', () => cancelItem(item));
  byId('upload-list').append(row);
  byId('uploads').hidden = false;
  if (saved) {
    updateItem(item, '正在恢复上传…');
    syncUploads(knownOffers);
    showRecovery(knownOffers);
  } else {
    sendOffer(item);
  }
}

function showRecovery(offers) {
  const available = new Set(offers.filter(({ state }) => state === 'pending' || state === 'approved' ||
    state === 'uploading').map(({ id }) => id));
  const records = resumableUploads().filter(({ id }) => available.has(id) && !queue.some((item) => item.id === id));
  const rendered = JSON.stringify(records);
  if (shownRecovery === rendered) return;
  shownRecovery = rendered;
  byId('resume-list').replaceChildren(...records.map((saved) => {
    const { row, meta } = fileRow(saved.name, '重新选择原文件以继续');
    const input = element('input', 'resume-picker');
    input.type = 'file';
    input.setAttribute('aria-label', '恢复 ' + saved.name);
    meta.after(input);
    const recover = async (file) => {
      if (!file) return;
      const current = generation;
      input.disabled = true;
      try {
        const same = file.name === saved.name && file.size === saved.size &&
          await fingerprint(file, () => generation !== current) === saved.digest;
        if (generation !== current) return;
        if (!same) throw new Error('Different file');
        if (!knownOffers.some(({ id, state }) => id === saved.id && state !== 'rejected' && state !== 'complete')) {
          throw new Error('Offer changed');
        }
        submitFile(file, saved);
      } catch {
        if (generation === current) message('无法恢复，请选择原文件并确认会话仍有效。', true);
      } finally {
        input.value = '';
        input.disabled = false;
      }
    };
    input.addEventListener('change', () => recover(input.files[0]));
    if (typeof window.showOpenFilePicker === 'function') savedHandle(saved.id).then((handle) => {
      if (!handle) return;
      const resume = element('button', '', '继续发送');
      resume.type = 'button';
      resume.addEventListener('click', async () => {
        try {
          if (await handle.requestPermission({ mode: 'read' }) !== 'granted') return input.click();
          await recover(await handle.getFile());
        } catch { message('无法读取原文件，请重新选择文件。', true); }
      });
      row.querySelector('.file-details').append(resume);
    });
    return row;
  }));
}

async function sendOffer(item) {
  item.failed = false;
  updateItem(item, '正在提交');
  try {
    if (item.keep && !item.digest) {
      updateItem(item, '正在校验文件…');
      item.digest = await fingerprint(item.file, () => item.cancelled || item.generation !== generation);
      if (item.cancelled || item.generation !== generation) return;
    }
    const response = await request('/api/offers', 'POST', {
      'X-File-Name': encodeURIComponent(item.file.name),
      'X-File-Size': String(item.file.size),
      'X-Upload-Batch': item.batch,
      'X-Upload-Directory': encodeURIComponent(item.directory),
      'X-Upload-Image': item.image ? '1' : '0',
    });
    if (!response.ok) throw new Error('offer');
    item.id = (await response.json()).id;
    if (item.generation !== generation) return;
    if (item.cancelled) {
      cancelItem(item);
    } else {
      if (item.keep) {
        if (!rememberUpload({ id: item.id, name: item.file.name, size: item.file.size, digest: item.digest })) {
          message('浏览器无法保存续传记录，本次仍可上传。', true);
        } else if (item.handle) {
          await savedHandle(item.id, item.handle);
        }
      }
      updateItem(item, '等待电脑确认');
    }
  } catch {
    if (item.cancelled || item.generation !== generation) return;
    item.failed = true;
    updateItem(item, '无法创建接收请求', undefined, true);
  }
}

async function cancelItem(item) {
  item.cancelled = true;
  item.request?.abort();
  updateItem(item, '已取消');
  if (!item.id) return;
  try {
    const response = await request(`/api/offers/${item.id}`, 'DELETE');
    if (response.status === 409) updateItem(item, '接收请求已结束');
    else if (!response.ok) throw new Error('cancel');
    forgetUpload(item.id);
  } catch {
    message('无法通知电脑取消，连接恢复后可结束会话。', true);
  }
}

function transfer(item) {
  const xhr = new XMLHttpRequest();
  item.request = xhr;
  const offset = item.offset;
  const end = Math.min(offset + chunkSize, item.file.size);
  updateItem(item, '正在发送', item.file.size ? offset / item.file.size * 100 : 0);
  const started = performance.now();
  xhr.open('PUT', `/api/upload/${item.id}`);
  xhr.timeout = 45000;
  xhr.setRequestHeader('X-TempShare', '1');
  xhr.setRequestHeader('Content-Type', 'application/octet-stream');
  xhr.setRequestHeader('X-Upload-Offset', String(offset));
  xhr.upload.onprogress = (event) => {
    if (!event.lengthComputable || !item.file.size || item.paused) return;
    const sent = offset + event.loaded;
    const percentage = Math.floor(sent / item.file.size * 100);
    const seconds = (performance.now() - started) / 1000;
    let status = `正在发送 ${percentage}%`;
    if (seconds >= 1 && event.loaded > 0 && sent < item.file.size) {
      const speed = event.loaded / seconds;
      const remaining = Math.ceil((item.file.size - sent) / speed);
      status += ` · ${formatSize(speed)}/s · 剩余约 ${remaining} 秒`;
    }
    updateItem(item, status, percentage);
  };
  xhr.onloadend = () => {
    item.request = undefined;
    if (item.cancelled || item.generation !== generation) return;
    if (xhr.status === 201) {
      item.attempts = 0;
      item.retryAt = 0;
      item.offset = end;
      if (end === item.file.size) {
        forgetUpload(item.id);
        item.finished = true;
        updateItem(item, '已保存到电脑');
      } else if (!item.paused) {
        transfer(item);
      }
    } else if (!item.paused) {
      if ((xhr.status === 0 || xhr.status === 503) && item.attempts < 4) {
        const retryAfter = Number(xhr.getResponseHeader('Retry-After')) || 0;
        item.retryAt = Date.now() + 1000 * Math.min(30, Math.max(retryAfter, 2 ** item.attempts++));
        knownOffersRevision = -1;
        updateItem(item, '等待连接恢复，随后继续');
        return;
      }
      item.failed = true;
      let code;
      try { code = JSON.parse(xhr.responseText).code; } catch { /* Empty response after disconnect. */ }
      const detail = {
        disk_space: '电脑的接收空间不足',
        storage_unavailable: '电脑无法写入接收目录',
        transfer_busy: '电脑正在处理其他传输',
        upload_unavailable: '等待授权或重新同步进度',
      };
      updateItem(item, detail[code] || '发送失败', undefined, true);
    }
  };
  xhr.send(item.file.slice(offset, end));
}

function syncUploads(offers) {
  const states = new Map(offers.map((offer) => [offer.id, offer]));
  let uploading = queue.filter((item) => item.request).length;
  for (const item of queue) {
    if (item.finished || item.cancelled || !item.id || item.request) continue;
    const offer = states.get(item.id);
    const state = offer?.state;
    if (state === 'rejected' || state === 'complete') {
      item.finished = true;
      forgetUpload(item.id);
      updateItem(item, state === 'complete' ? '已保存到电脑' : '电脑已拒绝');
    } else if (state === 'approved' && !item.failed && !item.paused && Date.now() >= item.retryAt) {
      item.approved = true;
      item.offset = offer.transferred;
      if (uploading < 3) {
        transfer(item);
        uploading++;
      } else {
        updateItem(item, '等待其他文件发送完成');
      }
    }
  }
}

async function poll() {
  if (polling || pairing) return;
  polling = true;
  const attempt = pairingAttempt;
  try {
    const pair = await request('/api/pair');
    if (attempt !== pairingAttempt) return;
    if (pair.status === 401) {
      forgetUpload();
      const error = await pair.json().catch(() => ({}));
      if (attempt === pairingAttempt) showConnection('', '', error.code);
      return;
    }
    if (!pair.ok) {
      if (connection === '') {
        message(pair.status === 403
          ? '此地址拒绝了当前网络中的设备。请核对桌面端显示的地址，并检查访客网络隔离。'
          : `无法访问 ${location.host}。请确认电脑仍在该局域网运行。`, true);
      }
      throw new Error('pair');
    }
    const { state, verification, upload, download, text, textRevision, castReady, filesRevision, offersRevision,
      computer, remainingSeconds } = await pair.json();
    if (attempt !== pairingAttempt) return;
    computerName = computer || '';
    sessionSeconds = remainingSeconds || 0;
    showConnection(state, verification);
    if (state !== 'approved') return;
    showCastAvailability(castReady === true);
    dropzone.hidden = !upload;
    byId('select-resumable').hidden = !upload || byId('resume-option').hidden || typeof window.showOpenFilePicker !== 'function';
    byId('directory-option').hidden = !upload || !('webkitdirectory' in byId('directory-input'));
    byId('upload-disabled').hidden = upload;
    byId('sync-image-label').hidden = !upload;
    byId('sync-form').hidden = !text;
    byId('sync-disabled').hidden = text;
    const refreshText = text && textRevision !== knownTextRevision;
    const [files, offers, entries] = await Promise.all([
      filesRevision === undefined || filesRevision !== knownFilesRevision ? request('/api/files?folders=1') : null,
      offersRevision === undefined || offersRevision !== knownOffersRevision ? request('/api/offers') : null,
      refreshText ? request('/api/text') : null,
    ]);
    if (attempt !== pairingAttempt) return;
    if (files?.status === 403 || offers?.status === 403) {
      showConnection('');
      return;
    }
    if ((files && !files.ok) || (offers && !offers.ok)) throw new Error('files');
    const [downloads, transfers, history] = await Promise.all([
      files?.json(), offers?.json(), entries?.ok ? entries.json() : null,
    ]);
    if (attempt !== pairingAttempt) return;
    downloadAllowed = download;
    if (downloads) {
      knownFilesRevision = filesRevision;
      showDownloads(downloads);
      if (text && !history) showMessages(syncMessages, true);
    }
    if (transfers) {
      knownOffersRevision = offersRevision;
      knownOffers = transfers;
      for (const record of resumableUploads()) {
        if (!knownOffers.some(({ id, state }) => id === record.id && state !== 'complete' && state !== 'rejected')) {
          forgetUpload(record.id);
        }
      }
    }
    syncUploads(knownOffers);
    showRecovery(knownOffers);
    if (history) {
      showMessages(history);
      knownTextRevision = textRevision;
    } else if (!text) {
      showMessages([]);
      knownTextRevision = -1;
    }
    if (interrupted) {
      message('');
      interrupted = false;
    }
  } catch {
    if (connection) {
      interrupted = true;
      message('与电脑的连接暂时中断，正在重试。', true);
    }
  } finally {
    polling = false;
  }
}

pairForm.addEventListener('submit', (event) => {
  event.preventDefault();
  askToPair(byId('pair-code').value);
});
byId('pair-code').addEventListener('input', (event) => {
  const input = event.currentTarget;
  input.value = input.value.normalize('NFKC').replace(/[^0-9]/g, '').slice(0, 6);
  byId('code-length').textContent = input.value.length ? `${input.value.length} / 6 位` : '';
});
trustedConnect.addEventListener('click', async () => {
  if (pairing || connection) return;
  pairing = true;
  pairingAttempt++;
  trustedConnect.disabled = true;
  try {
    const response = await request('/api/pair/trusted', 'POST');
    if (!response.ok) throw new Error('trusted');
  } catch {
    message('此浏览器的信任记录不可用，请确认电脑会话已开启或重新输入连接码。', true);
  } finally {
    trustedConnect.disabled = false;
    pairing = false;
    poll();
  }
});
byId('retry-connection').addEventListener('click', () => {
  if (connection) return poll();
  const code = byId('pair-code').value.trim();
  if (code) askToPair(code);
  else poll();
});
byId('feature-nav').addEventListener('click', (event) => {
  const button = event.target.closest('button[data-panel]');
  if (button) showTab(button.dataset.panel);
});
byId('sync-form').addEventListener('submit', sendText);
byId('disconnect').addEventListener('click', async (event) => {
  const button = event.currentTarget;
  const wasPending = connection === 'pending';
  button.disabled = true;
  try {
    const response = await request('/api/pair', 'DELETE');
    if (!response.ok && response.status !== 401) throw new Error('disconnect');
    pairingAttempt++;
    showConnection('');
    message(wasPending ? '已取消连接请求。' : '已断开连接。');
  } catch {
    message('无法断开连接，请在电脑上结束会话。', true);
  } finally {
    button.disabled = false;
  }
});
byId('sync-image').addEventListener('change', (event) => {
  if (connection === 'approved') {
    submitFiles(event.target.files, true);
  }
  event.target.value = '';
});
function startViewing(compatibility = typeof RTCPeerConnection === 'undefined') {
  watching = true;
  updateScreenLock();
  castStarted = Date.now();
  lastFrameAt = 0;
  useFrames = compatibility;
  castViewer = Array.from(crypto.getRandomValues(new Uint8Array(16)), (byte) => byte.toString(16).padStart(2, '0')).join('');
  castGeneration++;
  byId('cast-watch').textContent = '停止观看';
  byId('cast-mode').hidden = typeof RTCPeerConnection === 'undefined';
  byId('cast-mode').textContent = compatibility ? '重新尝试直连' : '切换兼容播放';
  castStatus('等待电脑开始投递。');
  refreshCast();
}
byId('cast-watch').addEventListener('click', () => watching ? stopViewing() : startViewing());
byId('cast-mode').addEventListener('click', async () => {
  if (!watching) return;
  const compatibility = !useFrames;
  const viewer = castViewer;
  stopViewing(false);
  const pending = castGeneration;
  try {
    await request('/api/cast/viewer', 'POST', { 'X-Cast-Watch': '0', 'X-Cast-Viewer': viewer });
    if (pending === castGeneration && connection === 'approved' && captureReady) startViewing(compatibility);
  } catch {
    castStatus('无法切换播放方式，请检查连接后重试。');
  }
});
byId('resume-watching').addEventListener('click', () => showTab('cast'));
byId('cast-fullscreen').addEventListener('click', async () => {
  const stage = byId('cast-stage');
  try {
    if (document.fullscreenElement) await document.exitFullscreen();
    else if (stage.requestFullscreen) await stage.requestFullscreen();
    else castVideo.webkitEnterFullscreen?.();
  } catch {
    castStatus('当前浏览器无法进入全屏。');
  }
});
byId('cast-audio').addEventListener('click', (event) => {
  castVideo.muted = !castVideo.muted;
  event.currentTarget.textContent = castVideo.muted ? '取消静音' : '静音';
});
byId('cast-awake-option').hidden = !navigator.wakeLock;
async function updateScreenLock() {
  if (!navigator.wakeLock) return;
  if (!watching || document.hidden || !byId('cast-awake').checked) {
    const lock = screenLock;
    screenLock = undefined;
    await lock?.release().catch(() => {});
    return;
  }
  if (screenLock && !screenLock.released) return;
  try {
    const lock = await navigator.wakeLock.request('screen');
    if (!watching || !byId('cast-awake').checked || document.hidden || (screenLock && !screenLock.released)) {
      await lock.release();
      return;
    }
    screenLock = lock;
  } catch {
    byId('cast-awake').checked = false;
    message('当前设备无法保持亮屏。', true);
  }
}
document.addEventListener('visibilitychange', updateScreenLock);
byId('cast-awake').addEventListener('change', updateScreenLock);
window.addEventListener('pagehide', () => stopViewing());
fileInput.addEventListener('change', () => {
  if (connection === 'approved') submitFiles(fileInput.files);
  fileInput.value = '';
});
byId('directory-input').addEventListener('change', (event) => {
  if (connection === 'approved') submitFiles(event.currentTarget.files);
  event.currentTarget.value = '';
});
byId('select-resumable').addEventListener('click', async () => {
  const current = generation;
  try {
    const handles = await window.showOpenFilePicker({ multiple: true });
    const files = await Promise.all(handles.map((handle) => handle.getFile()));
    if (current !== generation || connection !== 'approved') return;
    byId('remember-uploads').checked = true;
    const batch = Array.from(crypto.getRandomValues(new Uint8Array(16)),
      (value) => value.toString(16).padStart(2, '0')).join('');
    files.forEach((file, index) => submitFile(file, undefined, batch, false, handles[index]));
  } catch (error) {
    if (error.name !== 'AbortError') message('文件选择器不可用，可使用普通文件选择。', true);
  }
});
dropzone.addEventListener('dragover', (event) => {
  event.preventDefault();
  dropzone.classList.add('active');
});
dropzone.addEventListener('dragleave', () => dropzone.classList.remove('active'));
dropzone.addEventListener('drop', (event) => {
  event.preventDefault();
  dropzone.classList.remove('active');
  if (connection === 'approved') submitFiles(event.dataTransfer.files);
});

async function start() {
  const invite = location.hash.slice(1);
  if (invite) history.replaceState(null, '', location.pathname);
  await poll();
  if (!connection && /^[0-9a-f]{32}$/.test(invite)) await askToPair(invite);
  setInterval(() => {
    if (!document.hidden || watching || queue.some((item) => !item.cancelled && !item.finished)) poll();
  }, pollInterval);
  document.addEventListener('visibilitychange', () => { if (!document.hidden) poll(); });
  setInterval(refreshCast, 500);
}
start();
  updateQueueSummary();
