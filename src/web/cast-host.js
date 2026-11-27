import { gather, peerConnection, screenBitrate, screenOffer, timedFetch } from './rtc.js';

const mount = location.pathname.match(/^\/s\/[1-4](?=\/|$)/)?.[0] || '';
const keyStorage = 'tempshare-capture-' + location.pathname;
let key = location.hash.slice(1);
try {
  if (key) sessionStorage.setItem(keyStorage, key);
  else key = sessionStorage.getItem(keyStorage) || '';
} catch { /* 存储不可用时仍支持一次性本机授权。 */ }
if (key) history.replaceState(null, '', location.pathname);
const byId = (id) => document.getElementById(id);
const start = byId('start');
const stop = byId('stop');
const preview = byId('preview');
const preferences = ['quality', 'rate', 'audio', 'cast-profile', 'bitrate'];
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
let frameInterval = 1000 / 60;
let nextFrameAt = 0;
let frameScale = 1;
let frameQuality = .9;
let sentFrames = 0;
let sentWindowAt = performance.now();
let compatibilityFps = 0;
let lastStats;
let samplingStats = false;
let frameReader;
let frameTrack;
let frameClock;
let frameClockTimer;
const frameCanvas = document.createElement('canvas');
const frameContext = frameCanvas.getContext('2d', { alpha: false });

function status(text) {
  byId('status').textContent = text;
}

function host(path = 'host', body, headers = {}) {
  const method = path === 'host' ? 'GET' : 'POST';
  return timedFetch(mount + '/api/cast/' + path, {
    method, body, cache: 'no-store', keepalive: path === 'stop',
    headers: { 'X-Cast-Key': key, 'X-TempShare': '1', ...headers },
  }, 3500);
}

function closePeer() {
  const previous = peer;
  peer = undefined;
  previous?.close();
  lastStats = undefined;
}

function end(text, revoke = false) {
  ended = true;
  frameRevision = 0;
  stopFramePump();
  stopFrameClock();
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
    ? `目标 ${byId('rate').value} fps · 采集 ${width} × ${height}${frameRate ? ` · ${Math.round(frameRate)} fps` : ''}` : '';
}

function bitrateBudget() {
  const { width = 1920, height = 1080, frameRate = 60 } = stream?.getVideoTracks()[0]?.getSettings() || {};
  return screenBitrate(width, height, Math.min(frameRate, Number(byId('rate').value)), Number(byId('bitrate').value));
}

async function updateSender(sender) {
  if (!sender?.track || !sender.getParameters || !sender.setParameters) return;
  const parameters = sender.getParameters();
  const encoding = parameters.encodings?.[0];
  if (!encoding) return;
  encoding.maxBitrate = bitrateBudget();
  encoding.maxFramerate = Number(byId('rate').value);
  encoding.scaleResolutionDownBy = 1;
  parameters.degradationPreference = sender.track.contentHint === 'detail' ? 'maintain-resolution' : 'balanced';
  try { await sender.setParameters(parameters); } catch { /* 浏览器仍可使用自动码率控制。 */ }
}

async function applySettings() {
  const video = stream?.getVideoTracks()[0];
  if (video) {
    try {
      if ('contentHint' in video) video.contentHint = byId('cast-profile').value === 'smooth' ? 'motion' : 'detail';
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
    stopFramePump();
    closePeer();
    frameRevision = 0;
    stream = media;
    lastStream?.getTracks().forEach((track) => track.stop());
    const video = stream.getVideoTracks()[0];
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
  for (const track of stream.getTracks()) {
    const encoding = track.kind === 'video' ? { sendEncodings: [{ maxBitrate: bitrateBudget(), maxFramerate: Number(byId('rate').value), scaleResolutionDownBy: 1 }] } : {};
    current.addTransceiver(track, { direction: 'sendonly', streams: [stream], ...encoding });
  }
  current.addEventListener('connectionstatechange', () => {
    if (current !== peer) return;
    if (current.connectionState === 'connected') status('已连接观看设备。');
    if (current.connectionState === 'failed') {
      closePeer();
      nextOffer = Date.now() + 2000;
      status('连接中断，正在重试。');
    }
  });
  const offer = await gather(current, screenOffer(await current.createOffer(), bitrateBudget()));
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

async function publishFrame(source = preview) {
  const sourceWidth = source.displayWidth || source.videoWidth;
  const sourceHeight = source.displayHeight || source.videoHeight;
  if (!frameRevision || sendingFrame || !sourceWidth || !sourceHeight || !frameContext) return;
  const started = performance.now();
  if (started + 1 < nextFrameAt) return;
  const revision = frameRevision;
  sendingFrame = true;
  try {
    const scale = Math.min(1, 1920 / sourceWidth, 1080 / sourceHeight) * frameScale;
    const width = Math.max(1, Math.round(sourceWidth * scale));
    const height = Math.max(1, Math.round(sourceHeight * scale));
    if (frameCanvas.width !== width || frameCanvas.height !== height) {
      frameCanvas.width = width;
      frameCanvas.height = height;
    }
    frameContext.drawImage(source, 0, 0, width, height);
    const frame = await new Promise((resolve) => frameCanvas.toBlob(resolve, 'image/jpeg', frameQuality));
    if (revision === frameRevision && frame && frame.size <= 2 * 1024 * 1024) {
      const response = await host('frame', frame, { 'X-Cast-Revision': String(revision), 'Content-Type': 'image/jpeg' });
      if (response.ok) {
        const elapsed = performance.now() - started;
        frameInterval = 1000 / Math.min(60, Number(byId('rate').value));
        sentFrames++;
        const now = performance.now();
        if (now - sentWindowAt >= 1000) {
          compatibilityFps = Math.round(sentFrames * 1000 / (now - sentWindowAt));
          sentFrames = 0; sentWindowAt = now;
        }
        // 清晰度不因单个慢请求下降；网络慢时少发帧，避免文字持续被缩成低分辨率。
        if (elapsed < 35 && frameQuality < .9) frameQuality = Math.min(.9, frameQuality + .01);
      }
    } else if (frame?.size > 2 * 1024 * 1024) {
      if (frameQuality > .7) frameQuality -= .1;
      else frameScale = Math.max(.5, frameScale * .85);
    }
  } catch {
    frameInterval = Math.max(1000 / Math.min(60, Number(byId('rate').value)), 50);
  } finally {
    nextFrameAt = Math.max(started + frameInterval, performance.now());
    sendingFrame = false;
  }
}

function stopFramePump() {
  const reader = frameReader;
  frameReader = undefined;
  reader?.cancel().catch(() => {});
  frameTrack?.stop(); frameTrack = undefined;
}

function startFramePump() {
  if (frameReader || !stream || typeof MediaStreamTrackProcessor === 'undefined') return;
  let track;
  let reader;
  try {
    track = stream.getVideoTracks()[0].clone();
    reader = new MediaStreamTrackProcessor({ track, maxBufferSize: 1 }).readable.getReader();
  } catch { track?.stop(); return; }
  frameReader = reader; frameTrack = track;
  (async () => {
    try {
      while (frameReader === reader && !ended) {
        const { value, done } = await reader.read();
        if (done) break;
        try { if (frameReader === reader) await publishFrame(value); }
        finally { value.close(); }
      }
    } catch { /* 读取不可用时由定时后备路径继续送帧。 */ }
    finally {
      if (frameReader === reader) { frameReader = undefined; frameTrack = undefined; }
      reader.releaseLock(); track.stop();
    }
  })();
}

function startFrameClock() {
  if (frameClock || frameClockTimer || frameReader) return;
  // 后备采集不依赖预览画面是否正在合成；Worker避免普通后台页面定时器降到每秒一次。
  try {
    const url = URL.createObjectURL(new Blob(['let pending=false; onmessage=()=>pending=false; setInterval(()=>{if(!pending){pending=true;postMessage(0)}},8);'], { type: 'text/javascript' }));
    try { frameClock = new Worker(url); } finally { URL.revokeObjectURL(url); }
    const current = frameClock;
    current.onmessage = async () => {
      try { if (!frameReader) await publishFrame(); }
      finally { if (frameClock === current) current.postMessage(0); }
    };
  } catch {
    const next = () => { if (!ended) { if (!frameReader) publishFrame(); frameClockTimer = setTimeout(next, 16); } };
    next();
  }
}

function stopFrameClock() {
  frameClock?.terminate(); frameClock = undefined;
  clearTimeout(frameClockTimer); frameClockTimer = undefined;
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
    byId('target').textContent = signal.target ? `共享给：${signal.target}` : '';
    const revision = signal.frames ? signal.revision : 0;
    if (revision !== frameRevision) {
      frameRevision = revision;
      if (!revision) { stopFramePump(); stopFrameClock(); }
      frameInterval = 1000 / Math.min(60, Number(byId('rate').value));
      frameScale = 1;
      frameQuality = .9; compatibilityFps = 0; sentFrames = 0; sentWindowAt = performance.now();
      nextFrameAt = 0;
    }
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
      startFramePump();
      startFrameClock();
      status('兼容播放 · 无音频；可重新尝试直连以使用更高画质和帧率。');
      byId('transport-info').textContent = `兼容目标 ${Math.min(60, Number(byId('rate').value))} fps · 实际发送 ${frameCanvas.width} × ${frameCanvas.height} · ${compatibilityFps} fps`;
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
    } else if (peer && peer.connectionState !== 'connected' && Date.now() - negotiatingSince > 17000) {
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
  if (!ended) end('', false); // 释放本页采集；短期本机会话授权允许刷新后重新选择来源。
});
for (const id of ['quality', 'rate', 'bitrate']) {
  byId(id).addEventListener('change', () => {
    byId('cast-profile').value = 'custom';
    savePreferences();
    applySettings();
  });
}
byId('audio').addEventListener('change', () => {
  savePreferences();
  if (stream) byId('audio-info').textContent = '音频选项将在重新选择共享来源后生效。';
});
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
  setInterval(tick, 500);
  setInterval(sampleStats, 2000);
}

async function sampleStats() {
  if (!peer || samplingStats) return;
  samplingStats = true;
  const current = peer;
  try {
    const stats = await current.getStats();
    if (current !== peer) return;
    let video;
    let route;
    for (const report of stats.values()) {
      if (report.type === 'outbound-rtp' && report.kind === 'video') video = report;
      if (report.type === 'candidate-pair' && report.state === 'succeeded' && report.nominated) route = report;
    }
    if (video) {
      const rate = lastStats && video.timestamp > lastStats.timestamp
        ? (video.bytesSent - lastStats.bytesSent) * 8 / (video.timestamp - lastStats.timestamp) / 1000 : 0;
      const encodedFps = lastStats && video.timestamp > lastStats.timestamp ?
        Math.max(0, (video.framesEncoded - lastStats.framesEncoded) * 1000 / (video.timestamp - lastStats.timestamp)) : 0;
      const limitation = { cpu: '编码负载较高，可选流畅预设', bandwidth: '链路带宽受限，可降低码率或检查网络' }[video.qualityLimitationReason];
      byId('transport-info').textContent = `目标 ${byId('rate').value} fps · 实际发送 ${video.frameWidth || '?'} × ${video.frameHeight || '?'} · ${Math.round(video.framesPerSecond ?? encodedFps)} fps` +
        (rate ? ` · ${rate.toFixed(1)} Mbps` : '') +
        ` · 码率上限 ${Math.round(bitrateBudget() / 1e6)} Mbps` +
        (route?.currentRoundTripTime ? ` · RTT ${Math.round(route.currentRoundTripTime * 1000)} ms` : '') +
        (limitation ? ` · ${limitation}` : '');
      lastStats = { timestamp: video.timestamp, bytesSent: video.bytesSent, framesEncoded: video.framesEncoded };
    }
  } catch { /* 统计失败不打断媒体连接。 */ }
  finally { samplingStats = false; }
}
