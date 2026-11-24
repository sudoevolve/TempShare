'use strict';

import { gather, peerConnection } from './rtc.js';

const byId = (id) => document.getElementById(id);
const pairForm = byId('pair-form');
const dropzone = byId('dropzone');
const fileInput = byId('file-input');
const queue = [];
const maxFileSize = 1024 ** 3;
const chunkSize = 4 * 1024 ** 2;
const pollInterval = 1100;
let connection = '';
let generation = 0;
let shownDownloads = '';
let polling = false;
let pairing = false;
let pairingAttempt = 0;
let interrupted = false;
let tab = 'share';
let shownMessages = '';
let knownTextRevision = -1;
let watching = false;
let castPeer;
let castRevision = 0;
let castGeneration = 0;
let refreshingCast = false;
const trustedConnect = byId('trusted-connect');
trustedConnect.hidden = location.protocol !== 'https:';

function message(text, error = false) {
  byId('message').textContent = text;
  byId('message').classList.toggle('error', error);
}

function showConnection(state, verification = '') {
  const states = {
    '': ['未连接', '连接到电脑', '输入电脑上显示的连接码。'],
    pending: ['待确认', '等待电脑确认', '请在电脑上核对以下验证码并允许连接。'],
    approved: ['已连接', '', ''],
  };
  const [badge, title, description] = states[state];
  const changed = connection !== state;
  connection = state;
  byId('connection').dataset.state = state;
  byId('connection').textContent = badge;
  byId('disconnect').hidden = state === '';
  byId('connect-title').textContent = title;
  byId('connect-description').textContent = description;
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
    byId('upload-list').replaceChildren();
    byId('uploads').hidden = true;
    showDownloads([]);
    showMessages([]);
    knownTextRevision = -1;
    byId('sync-text').value = '';
    stopViewing(false);
    message('连接已结束或被拒绝，请使用电脑上显示的新连接码。', true);
  } else if (changed) {
    message('');
  }
}

function request(path, method = 'GET', headers = {}, body) {
  return fetch(path, { method, headers: method === 'GET' ? headers : { 'X-TempShare': '1', ...headers },
    body, cache: 'no-store', signal: AbortSignal.timeout(4500) });
}

function showTab(feature) {
  if (tab === feature) return;
  if (tab === 'cast') stopViewing();
  tab = feature;
  for (const button of byId('feature-nav').querySelectorAll('[data-panel]')) {
    const selected = button.dataset.panel === tab;
    button.setAttribute('aria-pressed', String(selected));
    byId(button.dataset.panel + '-panel').hidden = !selected;
  }
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
      message('连接码或邀请不可用。请核对电脑上的连接码。', true);
      return;
    }
    const data = await response.json();
    showConnection('pending', data.verification);
    byId('pair-code').value = '';
  } catch {
    message('无法连接到电脑，请检查 Wi-Fi 后重试。', true);
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
  const key = JSON.stringify(files);
  if (key === shownDownloads) return;
  shownDownloads = key;
  byId('no-files').hidden = files.length !== 0;
  byId('download-list').replaceChildren(...files.map((file) => {
    const { row } = fileRow(file.name, formatSize(file.size));
    const link = element('a', 'download', '下载');
    link.href = `/api/download/${encodeURIComponent(file.id)}`;
    link.download = file.name;
    link.setAttribute('aria-label', `下载 ${file.name}`);
    row.append(link);
    return row;
  }));
}

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

function showMessages(entries) {
  const key = JSON.stringify(entries);
  if (key === shownMessages) return;
  shownMessages = key;
  byId('sync-list').replaceChildren(...entries.slice().reverse().map((entry) => {
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
    content.append(element('p', 'file-meta', entry.desktop ? '电脑' : '本机'), body);
    const copy = element('button', '', '复制');
    copy.type = 'button';
    copy.addEventListener('click', () => copyText(entry.text));
    row.append(content, copy);
    return row;
  }));
}

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

function clearCastPeer() {
  const previous = castPeer;
  castPeer = undefined;
  previous?.close();
  castRevision = 0;
  const video = byId('cast-video');
  video.srcObject = null;
  video.hidden = true;
}

function stopViewing(notify = true) {
  const wasWatching = watching;
  watching = false;
  castGeneration++;
  clearCastPeer();
  castStatus('');
  byId('cast-watch').textContent = '开始观看';
  if (notify && wasWatching) fetch('/api/cast/viewer', {
    method: 'POST', headers: { 'X-TempShare': '1', 'X-Cast-Watch': '0' }, keepalive: true,
  }).catch(() => {});
}

async function refreshCast() {
  if (!watching || refreshingCast) return;
  const current = castGeneration;
  refreshingCast = true;
  try {
    const response = await request('/api/cast/viewer', 'POST', { 'X-Cast-Watch': '1' });
    if (current !== castGeneration) return;
    if (!response.ok) throw new Error('viewer');
    const signal = await response.json();
    if (current !== castGeneration) return;
    if (!signal.offer) {
      clearCastPeer();
      castStatus('等待电脑开始投递。');
      return;
    }
    if (signal.revision === castRevision) return;
    clearCastPeer();
    const peer = peerConnection();
    castPeer = peer;
    castRevision = signal.revision;
    peer.addEventListener('track', (event) => {
      if (peer !== castPeer) return;
      const video = byId('cast-video');
      video.srcObject = event.streams[0] || new MediaStream([event.track]);
      video.hidden = false;
      video.play().catch(() => {});
      castStatus('');
    });
    peer.addEventListener('connectionstatechange', () => {
      if (peer === castPeer && peer.connectionState === 'failed') castStatus('媒体连接中断，正在等待电脑重连。');
    });
    await peer.setRemoteDescription({ type: 'offer', sdp: signal.offer });
    const answer = await gather(peer, await peer.createAnswer());
    if (peer !== castPeer) return;
    const result = await request('/api/cast/answer', 'POST', { 'X-Cast-Revision': String(signal.revision) }, answer);
    if (!result.ok) throw new Error('answer');
  } catch {
    if (current === castGeneration) {
      clearCastPeer();
      castStatus('正在等待可用的局域网媒体连接。');
    }
  } finally {
    refreshingCast = false;
  }
}

function updateItem(item, status, progress, failed = false) {
  item.meta.textContent = `${formatSize(item.file.size)} · ${status}`;
  item.progress.hidden = progress === undefined;
  item.progress.value = progress ?? 0;
  item.retry.hidden = !failed;
  item.cancel.hidden = item.finished || item.cancelled;
  item.pause.hidden = item.finished || item.cancelled || !item.approved || item.failed;
  item.pause.textContent = item.paused ? '继续' : '暂停';
}

function submitFile(file) {
  if (file.size > maxFileSize) {
    message(`${file.name} 超过 1 GiB，未加入队列。`, true);
    return;
  }
  if (queue.length >= 100) {
    message('本会话的文件队列已满。', true);
    return;
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
  const item = { file, meta, progress, retry, pause, cancel, generation, offset: 0 };
  queue.push(item);
  retry.addEventListener('click', () => {
    if (item.request || item.cancelled) return;
    if (item.id) {
      item.failed = false;
      updateItem(item, '等待上传');
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
  sendOffer(item);
}

async function sendOffer(item) {
  item.failed = false;
  updateItem(item, '正在提交');
  try {
    const response = await request('/api/offers', 'POST', {
      'X-File-Name': encodeURIComponent(item.file.name),
      'X-File-Size': String(item.file.size),
    });
    if (!response.ok) throw new Error('offer');
    item.id = (await response.json()).id;
    if (item.generation !== generation) return;
    if (item.cancelled) {
      cancelItem(item);
    } else {
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
      item.offset = end;
      if (end === item.file.size) {
        item.finished = true;
        updateItem(item, '已保存到电脑');
      } else if (!item.paused) {
        transfer(item);
      }
    } else if (!item.paused) {
      item.failed = true;
      updateItem(item, '发送失败', undefined, true);
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
      updateItem(item, state === 'complete' ? '已保存到电脑' : '电脑已拒绝');
    } else if (state === 'approved' && !item.failed && !item.paused) {
      item.approved = true;
      item.offset = Math.max(item.offset, offer.transferred);
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
      showConnection('');
      return;
    }
    if (!pair.ok) throw new Error('pair');
    const { state, verification, upload, download, text, textRevision } = await pair.json();
    if (attempt !== pairingAttempt) return;
    showConnection(state, verification);
    if (state !== 'approved') return;
    dropzone.hidden = !upload;
    byId('upload-disabled').hidden = upload;
    byId('sync-image-label').hidden = !upload;
    byId('sync-form').hidden = !text;
    byId('sync-disabled').hidden = text;
    const refreshText = text && tab === 'sync' && textRevision !== knownTextRevision;
    const [files, offers, entries] = await Promise.all([
      request('/api/files'), request('/api/offers'), refreshText ? request('/api/text') : null,
    ]);
    if (attempt !== pairingAttempt) return;
    if (files.status === 403 || offers.status === 403) {
      showConnection('');
      return;
    }
    if (!files.ok || !offers.ok) throw new Error('files');
    const [downloads, transfers, history] = await Promise.all([
      files.json(), offers.json(), entries?.ok ? entries.json() : null,
    ]);
    if (attempt !== pairingAttempt) return;
    showDownloads(downloads);
    if (!download) byId('no-files').textContent = '电脑关闭了文件下载权限。';
    else byId('no-files').textContent = '电脑分享文件后会显示在这里。';
    syncUploads(transfers);
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
byId('feature-nav').addEventListener('click', (event) => {
  if (event.target.dataset.panel) showTab(event.target.dataset.panel);
});
byId('sync-form').addEventListener('submit', sendText);
byId('disconnect').addEventListener('click', async (event) => {
  const button = event.currentTarget;
  button.disabled = true;
  try {
    const response = await request('/api/pair', 'DELETE');
    if (!response.ok && response.status !== 401) throw new Error('disconnect');
    pairingAttempt++;
    showConnection('');
  } catch {
    message('无法断开连接，请在电脑上结束会话。', true);
  } finally {
    button.disabled = false;
  }
});
byId('sync-image').addEventListener('change', (event) => {
  if (connection === 'approved') {
    Array.from(event.target.files).forEach(submitFile);
    showTab('share');
  }
  event.target.value = '';
});
byId('cast-watch').addEventListener('click', () => {
  if (watching) return stopViewing();
  if (typeof RTCPeerConnection === 'undefined') return castStatus('当前浏览器不支持 WebRTC。');
  watching = true;
  castGeneration++;
  byId('cast-watch').textContent = '停止观看';
  castStatus('等待电脑开始投递。');
  refreshCast();
});
window.addEventListener('pagehide', () => stopViewing());
fileInput.addEventListener('change', () => {
  if (connection === 'approved') Array.from(fileInput.files).forEach(submitFile);
  fileInput.value = '';
});
dropzone.addEventListener('dragover', (event) => {
  event.preventDefault();
  dropzone.classList.add('active');
});
dropzone.addEventListener('dragleave', () => dropzone.classList.remove('active'));
dropzone.addEventListener('drop', (event) => {
  event.preventDefault();
  dropzone.classList.remove('active');
  if (connection === 'approved') Array.from(event.dataTransfer.files).forEach(submitFile);
});

async function start() {
  const invite = location.hash.slice(1);
  if (invite) history.replaceState(null, '', location.pathname);
  await poll();
  if (!connection && /^[0-9a-f]{32}$/.test(invite)) await askToPair(invite);
  setInterval(poll, pollInterval);
  setInterval(refreshCast, 900);
}
start();
