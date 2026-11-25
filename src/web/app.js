'use strict';

import { gather, peerConnection } from './rtc.js';
import { fingerprint, forgetUpload, rememberUpload, resumableUploads } from './resume.js';

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
  const disconnect = byId('disconnect');
  disconnect.hidden = state === '';
  disconnect.textContent = state === 'pending' ? '取消请求' : '断开';
  byId('retry-connection').hidden = state !== '' || pairing;
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
    knownFilesRevision = -1;
    knownOffersRevision = -1;
    knownOffers = [];
    forgetUpload();
    showRecovery([]);
    byId('sync-text').value = '';
    showCastAvailability(false);
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
  const video = byId('cast-video');
  video.srcObject = null;
  video.hidden = true;
}

function stopViewing(notify = true) {
  const wasWatching = watching;
  const viewer = castViewer;
  watching = false;
  castGeneration++;
  clearCastPeer();
  showCastAvailability(captureReady);
  byId('cast-watch').textContent = '开始观看';
  if (notify && wasWatching) fetch('/api/cast/viewer', {
    method: 'POST', headers: { 'X-TempShare': '1', 'X-Cast-Watch': '0', 'X-Cast-Viewer': viewer }, keepalive: true,
  }).catch(() => {});
}

async function refreshCast() {
  if (!watching || refreshingCast) return;
  const current = castGeneration;
  refreshingCast = true;
  try {
    const response = await request('/api/cast/viewer', 'POST', { 'X-Cast-Watch': '1', 'X-Cast-Viewer': castViewer });
    if (current !== castGeneration) return;
    if (response.status === 409) {
      stopViewing(false);
      castStatus('已有另一个标签页正在观看。');
      return;
    }
    if (!response.ok) throw new Error('viewer');
    const signal = await response.json();
    if (current !== castGeneration) return;
    if (!signal.offer) {
      clearCastPeer();
      if (!signal.ready) {
        stopViewing(false);
        showCastAvailability(false);
      } else {
        castStatus('正在连接电脑…');
      }
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
    const result = await request('/api/cast/answer', 'POST', {
      'X-Cast-Revision': String(signal.revision), 'X-Cast-Viewer': castViewer,
    }, answer);
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

function submitFile(file, saved) {
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
  const item = { file, row, meta, progress, retry, pause, cancel, generation, offset: 0,
    id: saved?.id, keep: byId('remember-uploads').checked && !byId('resume-option').hidden };
  queue.push(item);
  retry.addEventListener('click', () => {
    if (item.request || item.cancelled) return;
    if (item.id) {
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
    input.addEventListener('change', async () => {
      const [file] = input.files;
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
    });
    if (!response.ok) throw new Error('offer');
    item.id = (await response.json()).id;
    if (item.generation !== generation) return;
    if (item.cancelled) {
      cancelItem(item);
    } else {
      if (item.keep && !rememberUpload({ id: item.id, name: item.file.name, size: item.file.size, digest: item.digest })) {
        message('浏览器无法保存续传记录，本次仍可上传。', true);
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
      item.offset = end;
      if (end === item.file.size) {
        forgetUpload(item.id);
        item.finished = true;
        updateItem(item, '已保存到电脑');
      } else if (!item.paused) {
        transfer(item);
      }
    } else if (!item.paused) {
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
      forgetUpload();
      showConnection('');
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
    const { state, verification, upload, download, text, textRevision, castReady, filesRevision, offersRevision } = await pair.json();
    if (attempt !== pairingAttempt) return;
    showConnection(state, verification);
    if (state !== 'approved') return;
    showCastAvailability(castReady === true);
    dropzone.hidden = !upload;
    byId('upload-disabled').hidden = upload;
    byId('sync-image-label').hidden = !upload;
    byId('sync-form').hidden = !text;
    byId('sync-disabled').hidden = text;
    const refreshText = text && tab === 'sync' && textRevision !== knownTextRevision;
    const [files, offers, entries] = await Promise.all([
      filesRevision === undefined || filesRevision !== knownFilesRevision ? request('/api/files') : null,
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
    if (downloads) {
      knownFilesRevision = filesRevision;
      showDownloads(downloads);
    }
    if (!download) byId('no-files').textContent = '电脑关闭了文件下载权限。';
    else byId('no-files').textContent = '暂无共享文件';
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
    Array.from(event.target.files).forEach((file) => submitFile(file));
    showTab('share');
  }
  event.target.value = '';
});
byId('cast-watch').addEventListener('click', () => {
  if (watching) return stopViewing();
  if (typeof RTCPeerConnection === 'undefined') return castStatus('当前浏览器不支持 WebRTC。');
  watching = true;
  castViewer = Array.from(crypto.getRandomValues(new Uint8Array(16)), (byte) => byte.toString(16).padStart(2, '0')).join('');
  castGeneration++;
  byId('cast-watch').textContent = '停止观看';
  castStatus('等待电脑开始投递。');
  refreshCast();
});
window.addEventListener('pagehide', () => stopViewing());
fileInput.addEventListener('change', () => {
  if (connection === 'approved') Array.from(fileInput.files).forEach((file) => submitFile(file));
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
  if (connection === 'approved') Array.from(event.dataTransfer.files).forEach((file) => submitFile(file));
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
  setInterval(refreshCast, 900);
}
start();
