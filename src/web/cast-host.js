import { gather, peerConnection, timedFetch } from './rtc.js';

const key = location.hash.slice(1);
if (key) history.replaceState(null, '', location.pathname);
const byId = (id) => document.getElementById(id);
const start = byId('start');
const stop = byId('stop');
const preview = byId('preview');
const preferences = ['quality', 'rate', 'audio', 'cast-profile'];
for (const id of preferences) {
  try {
    const saved = localStorage.getItem('cast-' + id);
    const control = byId(id);
    if (id === 'audio') control.checked = saved === 'true';
    else if ([...control.options].some(({ value }) => value === saved)) control.value = saved;
  } catch { /* Storage may be blocked. */ }
}

function savePreferences() {
  try {
    for (const id of preferences) {
      const control = byId(id);
      localStorage.setItem('cast-' + id, id === 'audio' ? String(control.checked) : control.value);
    }
  } catch { /* Preferences still apply to this tab. */ }
}
let stream;
let peer;
let ended = false;
let busy = false;
let capturing = false;
let failures = 0;
let nextOffer = 0;
let negotiatingSince = 0;
let frameRevision = 0;
let sendingFrame = false;
const frameCanvas = document.createElement('canvas');
const frameContext = frameCanvas.getContext('2d', { alpha: false });

function status(text) {
  byId('status').textContent = text;
}

function host(path = 'host', body, headers = {}) {
  const method = path === 'host' ? 'GET' : 'POST';
  return timedFetch('/api/cast/' + path, {
    method, body, cache: 'no-store', keepalive: path === 'stop',
    headers: { 'X-Cast-Key': key, 'X-TempShare': '1', ...headers },
  }, 3500);
}

function closePeer() {
  const previous = peer;
  peer = undefined;
  previous?.close();
}

function end(text, revoke = false) {
  ended = true;
  frameRevision = 0;
  closePeer();
  stream?.getTracks().forEach((track) => track.stop());
  preview.srcObject = null;
  preview.hidden = true;
  start.disabled = true;
  stop.hidden = true;
  status(text);
  if (revoke) host('stop').catch(() => {});
}

function settings() {
  const height = Number(byId('quality').value);
  const frameRate = Number(byId('rate').value);
  return {
    ...(height ? { height: { ideal: height, max: height } } : {}),
    frameRate: { ideal: frameRate, max: frameRate },
  };
}

function updateCaptureInfo() {
  const track = stream?.getVideoTracks()[0];
  if (!track) return;
  const { width, height, frameRate } = track.getSettings();
  byId('capture-info').textContent = width && height
    ? `采集规格 ${width} × ${height}${frameRate ? ` · ${Math.round(frameRate)} fps` : ''}` : '';
}

async function updateSender(sender) {
  if (!sender?.track || !sender.getParameters || !sender.setParameters) return;
  const parameters = sender.getParameters();
  const encoding = parameters.encodings?.[0];
  if (!encoding) return;
  const { width = 1920, height = 1080, frameRate = 30 } = sender.track.getSettings();
  encoding.maxBitrate = Math.round(Math.min(36e6, Math.max(2e6, width * height * frameRate * 0.075)));
  parameters.degradationPreference = 'maintain-resolution';
  try { await sender.setParameters(parameters); } catch { /* 浏览器仍可使用自动码率控制。 */ }
}

async function applySettings() {
  const video = stream?.getVideoTracks()[0];
  if (video) {
    try {
      await video.applyConstraints(settings());
      updateCaptureInfo();
      await updateSender(peer?.getSenders().find((sender) => sender.track === video));
    } catch {
      status('当前共享来源无法应用所选画质或帧率。');
    }
  }
}

async function capture() {
  capturing = true;
  start.disabled = true;
  try {
    const media = await navigator.mediaDevices.getDisplayMedia({
      video: { frameRate: { ideal: Number(byId('rate').value) } }, audio: byId('audio').checked,
    });
    if (ended) {
      media.getTracks().forEach((track) => track.stop());
      return;
    }
    const lastStream = stream;
    closePeer();
    frameRevision = 0;
    stream = media;
    lastStream?.getTracks().forEach((track) => track.stop());
    const video = stream.getVideoTracks()[0];
    if ('contentHint' in video) video.contentHint = 'detail';
    video.addEventListener('ended', () => {
      if (stream === media) end('投递已停止。', true);
    }, { once: true });
    preview.srcObject = stream;
    preview.hidden = false;
    stop.hidden = false;
    start.textContent = '更换共享来源';
    byId('audio-info').textContent = media.getAudioTracks().length ? '已捕获共享音频' :
      byId('audio').checked ? '选择的来源未提供音轨' : '未请求共享音频';
    status('等待配对设备开始观看。');
    await applySettings();
  } catch (error) {
    if (!ended) {
      start.disabled = false;
      status({
        NotAllowedError: '来源选择已取消或权限被拒绝。可重新选择，或检查浏览器录屏权限。',
        NotReadableError: '系统无法读取所选画面。请尝试其他窗口或屏幕。',
        InvalidStateError: '请先将控制台切到前台，再选择来源。',
      }[error.name] || '无法获取画面，请重新选择共享来源。');
    }
  } finally {
    capturing = false;
    if (!ended) start.disabled = false;
  }
}

async function negotiate() {
  const current = peerConnection();
  peer = current;
  negotiatingSince = Date.now();
  for (const track of stream.getTracks()) current.addTrack(track, stream);
  current.addEventListener('connectionstatechange', () => {
    if (current !== peer) return;
    if (current.connectionState === 'connected') status('已连接观看设备。');
    if (current.connectionState === 'failed') {
      closePeer();
      nextOffer = Date.now() + 2000;
      status('连接中断，正在重试。');
    }
  });
  const offer = await gather(current, await current.createOffer());
  if (current !== peer || ended) return;
  const response = await host('offer', offer);
  if (!response.ok) {
    closePeer();
    nextOffer = Date.now() + 2200;
    status('正在等待可用的局域网媒体连接。');
  } else {
    status('正在建立媒体连接…');
  }
}

async function publishFrame() {
  if (!frameRevision || sendingFrame || preview.readyState < 2 || !preview.videoWidth || !frameContext) return;
  const revision = frameRevision;
  sendingFrame = true;
  try {
    const scale = Math.min(1, 1280 / preview.videoWidth, 720 / preview.videoHeight);
    const width = Math.max(1, Math.round(preview.videoWidth * scale));
    const height = Math.max(1, Math.round(preview.videoHeight * scale));
    if (frameCanvas.width !== width || frameCanvas.height !== height) {
      frameCanvas.width = width;
      frameCanvas.height = height;
    }
    frameContext.drawImage(preview, 0, 0, width, height);
    let frame = await new Promise((resolve) => frameCanvas.toBlob(resolve, 'image/jpeg', .67));
    if (frame?.size > 384 * 1024) {
      frame = await new Promise((resolve) => frameCanvas.toBlob(resolve, 'image/jpeg', .40));
    }
    if (revision === frameRevision && frame && frame.size <= 384 * 1024) {
      await host('frame', frame, { 'X-Cast-Revision': String(revision), 'Content-Type': 'image/jpeg' });
    }
  } catch {
    // 下一帧继续尝试，控制请求负责判断会话是否仍然有效。
  } finally {
    sendingFrame = false;
  }
}

async function tick() {
  if (busy || ended) return;
  busy = true;
  try {
    const response = await host();
    if (response.status === 403) {
      end('投递授权已结束，请从桌面端重新开启。');
      return;
    }
    if (!response.ok) throw new Error('cast host');
    failures = 0;
    const signal = await response.json();
    frameRevision = signal.frames ? signal.revision : 0;
    if (!stream) {
      start.disabled = capturing;
      return;
    }
    if (!signal.ready) {
      const ready = await host('ready');
      if (!ready.ok) throw new Error('cast readiness');
    }
    if (frameRevision) {
      closePeer();
      status('对方正在使用兼容播放。');
    } else if (!signal.viewing) {
      closePeer();
      status('等待配对设备开始观看。');
    } else if (!peer && Date.now() >= nextOffer) {
      await negotiate();
    } else if (peer && !signal.offer) {
      closePeer();
    } else if (signal.answer && peer?.signalingState === 'have-local-offer') {
      await peer.setRemoteDescription({ type: 'answer', sdp: signal.answer });
      await updateSender(peer?.getSenders().find((sender) => sender.track?.kind === 'video'));
    } else if (peer && peer.connectionState !== 'connected' && Date.now() - negotiatingSince > 12000) {
      closePeer();
      nextOffer = Date.now() + 1200;
    }
  } catch {
    closePeer();
    if (++failures >= 2) end('与桌面端失去连接，投递已停止。', true);
    else status('正在重新连接桌面端。');
  } finally {
    busy = false;
  }
}

start.addEventListener('click', capture);
stop.addEventListener('click', () => end('投递已停止。', true));
window.addEventListener('pagehide', () => {
  if (!ended) end('', true);
});
for (const id of ['quality', 'rate']) {
  byId(id).addEventListener('change', () => {
    byId('cast-profile').value = 'custom';
    savePreferences();
    applySettings();
  });
}
byId('audio').addEventListener('change', savePreferences);
byId('cast-profile').addEventListener('change', (event) => {
  const presets = { smooth: ['720', '60'], clear: ['1440', '30'], original: ['0', '60'] };
  const preset = presets[event.currentTarget.value];
  if (preset) [byId('quality').value, byId('rate').value] = preset;
  savePreferences();
  applySettings();
});

if (!/^[0-9a-f]{32}$/.test(key)) end('请从 TempShare 桌面端打开本页。');
else if (!navigator.mediaDevices?.getDisplayMedia || typeof RTCPeerConnection === 'undefined') {
  end('当前浏览器不支持屏幕捕获，请使用支持屏幕录制的桌面浏览器。', true);
} else {
  await tick();
  if (!ended && failures === 0) start.disabled = false;
  setInterval(tick, 900);
  setInterval(publishFrame, 100);
}
