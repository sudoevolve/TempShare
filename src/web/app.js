'use strict';

const byId = (id) => document.getElementById(id);
const pairForm = byId('pair-form');
const dropzone = byId('dropzone');
const fileInput = byId('file-input');
const queue = [];
const maxFileSize = 1024 ** 3;
const pollInterval = 1100;
let connection = '';
let generation = 0;
let shownDownloads = '';
let polling = false;
let pairing = false;
let pairingAttempt = 0;
let interrupted = false;

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
  byId('connect-title').textContent = title;
  byId('connect-description').textContent = description;
  byId('connect-panel').hidden = state === 'approved';
  byId('share-panel').hidden = state !== 'approved';
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
    message('连接已结束或被拒绝，请使用电脑上显示的新连接码。', true);
  } else if (changed) {
    message('');
  }
}

function request(path, method = 'GET', headers = {}) {
  const options = method === 'GET' ? {} : { method, headers: { 'X-TempShare': '1', ...headers } };
  return fetch(path, { ...options, cache: 'no-store' });
}

async function askToPair(invitation) {
  if (pairing || connection) return;
  pairing = true;
  pairingAttempt++;
  pairForm.querySelector('button').disabled = true;
  try {
    const response = await request('/api/pair', 'POST', { 'X-Invitation': invitation });
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

function updateItem(item, status, progress, failed = false) {
  item.meta.textContent = `${formatSize(item.file.size)} · ${status}`;
  item.progress.hidden = progress === undefined;
  item.progress.value = progress ?? 0;
  item.retry.hidden = !failed;
  item.cancel.hidden = item.finished || item.cancelled;
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
  const progress = document.createElement('progress');
  progress.max = 100;
  progress.setAttribute('aria-label', `${file.name} 发送进度`);
  controls.append(retry, cancel);
  row.append(controls, progress);
  const item = { file, meta, progress, retry, cancel, generation };
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
  updateItem(item, '正在发送', 0);
  const started = performance.now();
  xhr.open('PUT', `/api/upload/${item.id}`);
  xhr.setRequestHeader('X-TempShare', '1');
  xhr.setRequestHeader('Content-Type', 'application/octet-stream');
  xhr.upload.onprogress = (event) => {
    if (!event.lengthComputable || !event.total) return;
    const percentage = Math.floor(event.loaded / event.total * 100);
    const seconds = (performance.now() - started) / 1000;
    let status = `正在发送 ${percentage}%`;
    if (seconds >= 1 && event.loaded > 0 && event.loaded < event.total) {
      const speed = event.loaded / seconds;
      const remaining = Math.ceil((event.total - event.loaded) / speed);
      status += ` · ${formatSize(speed)}/s · 剩余约 ${remaining} 秒`;
    }
    updateItem(item, status, percentage);
  };
  xhr.onloadend = () => {
    item.request = undefined;
    if (item.cancelled || item.generation !== generation) return;
    item.finished = xhr.status === 201;
    item.failed = !item.finished;
    updateItem(item, item.finished ? '已保存到电脑' : '发送失败', undefined, item.failed);
  };
  xhr.send(item.file);
}

function syncUploads(offers) {
  const states = new Map(offers.map((offer) => [offer.id, offer.state]));
  let uploading = queue.filter((item) => item.request).length;
  for (const item of queue) {
    if (item.finished || item.cancelled || !item.id || item.request) continue;
    const state = states.get(item.id);
    if (state === 'rejected' || state === 'complete') {
      item.finished = true;
      updateItem(item, state === 'complete' ? '已保存到电脑' : '电脑已拒绝');
    } else if (state === 'approved' && !item.failed) {
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
    const { state, verification } = await pair.json();
    showConnection(state, verification);
    if (state !== 'approved') return;
    const [files, offers] = await Promise.all([request('/api/files'), request('/api/offers')]);
    if (files.status === 403 || offers.status === 403) {
      showConnection('');
      return;
    }
    if (!files.ok || !offers.ok) throw new Error('files');
    showDownloads(await files.json());
    syncUploads(await offers.json());
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
}
start();
