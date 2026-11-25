import { gather, peerConnection } from './rtc.js';

const key = location.hash.slice(1);
if (key) history.replaceState(null, '', location.pathname);
const byId = (id) => document.getElementById(id);
const start = byId('start');
const stop = byId('stop');
const preview = byId('preview');
let stream;
let peer;
let ended = false;
let busy = false;
let capturing = false;
let failures = 0;
let nextOffer = 0;
let negotiatingSince = 0;

function status(text) {
  byId('status').textContent = text;
}

function host(path = 'host', body) {
  const method = path === 'host' ? 'GET' : 'POST';
  return fetch('/api/cast/' + path, {
    method, body, cache: 'no-store', keepalive: path === 'stop',
    headers: { 'X-Cast-Key': key, 'X-TempShare': '1' },
    signal: AbortSignal.timeout(3500),
  });
}

function closePeer() {
  const previous = peer;
  peer = undefined;
  previous?.close();
}

function end(text, revoke = false) {
  ended = true;
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
  return { width: { ideal: Math.round(height * 16 / 9) }, height: { ideal: height }, frameRate: { ideal: Number(byId('rate').value) } };
}

async function applySettings() {
  const video = stream?.getVideoTracks()[0];
  if (video) {
    try {
      await video.applyConstraints(settings());
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
      video: settings(), audio: byId('audio').checked,
    });
    if (ended) {
      media.getTracks().forEach((track) => track.stop());
      return;
    }
    stream = media;
    stream.getVideoTracks()[0].addEventListener('ended', () => end('投递已停止。', true), { once: true });
    preview.srcObject = stream;
    preview.hidden = false;
    stop.hidden = false;
    byId('audio').disabled = true;
    status('等待配对设备开始观看。');
  } catch {
    if (!ended) {
      start.disabled = false;
      status('未能获取画面。请检查浏览器的屏幕录制权限。');
    }
  } finally {
    capturing = false;
  }
}

async function negotiate() {
  const current = peerConnection();
  peer = current;
  negotiatingSince = Date.now();
  for (const track of stream.getTracks()) {
    current.addTransceiver(track, { direction: 'sendonly', streams: [stream] });
  }
  current.addEventListener('connectionstatechange', () => {
    if (current !== peer) return;
    if (current.connectionState === 'connected') status('对方正在观看。');
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
    if (!stream) {
      start.disabled = capturing;
      return;
    }
    if (!signal.ready) {
      const ready = await host('ready');
      if (!ready.ok) throw new Error('cast readiness');
    }
    if (!signal.viewing) {
      closePeer();
      status('等待配对设备开始观看。');
    } else if (!peer && Date.now() >= nextOffer) {
      await negotiate();
    } else if (peer && !signal.offer) {
      closePeer();
    } else if (signal.answer && peer?.signalingState === 'have-local-offer') {
      await peer.setRemoteDescription({ type: 'answer', sdp: signal.answer });
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
byId('quality').addEventListener('change', applySettings);
byId('rate').addEventListener('change', applySettings);

if (!/^[0-9a-f]{32}$/.test(key)) end('请从 TempShare 桌面端打开本页。');
else if (!navigator.mediaDevices?.getDisplayMedia || typeof RTCPeerConnection === 'undefined') {
  end('当前浏览器不支持屏幕捕获，请使用支持屏幕录制的桌面浏览器。', true);
} else {
  await tick();
  if (!ended && failures === 0) start.disabled = false;
  setInterval(tick, 900);
}
