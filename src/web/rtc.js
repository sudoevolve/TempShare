export function peerConnection() {
  return new RTCPeerConnection({ iceServers: [], bundlePolicy: 'max-bundle' });
}

// 在首次协商前给出局域网屏幕编码预算，避免先用低码率起步后才设置发送参数。
export function screenBitrate(width, height, frameRate, megabits = 0) {
  return Math.round(megabits > 0 ? megabits * 1e6 : Math.min(120e6, Math.max(4e6, width * height * frameRate * .18)));
}

export function screenOffer(description, bitrate) {
  const lines = description.sdp.split(/\r?\n/);
  let video = false;
  const payloads = new Set();
  const existing = new Set();
  for (const line of lines) {
    if (line.startsWith('m=')) video = line.startsWith('m=video ');
    if (!video) continue;
    const codec = line.match(/^a=rtpmap:(\d+) (?:VP8|VP9|H264|AV1)\/90000$/i);
    if (codec) payloads.add(codec[1]);
    const format = line.match(/^a=fmtp:(\d+) /);
    if (format) existing.add(format[1]);
  }
  const maximum = Math.round(bitrate / 1000);
  const start = Math.min(32000, Math.max(2000, Math.round(maximum * .65)));
  const hints = `x-google-start-bitrate=${start};x-google-max-bitrate=${maximum}`;
  video = false;
  const result = [];
  for (const line of lines) {
    if (line.startsWith('m=')) video = line.startsWith('m=video ');
    const format = video && line.match(/^a=fmtp:(\d+) (.*)$/);
    if (format && payloads.has(format[1])) {
      const parameters = format[2].split(';').filter(item => !/^\s*x-google-(?:start|max)-bitrate=/i.test(item));
      result.push(`a=fmtp:${format[1]} ${parameters.join(';')};${hints}`);
    } else {
      result.push(line);
      const codec = video && line.match(/^a=rtpmap:(\d+) /);
      if (codec && payloads.has(codec[1]) && !existing.has(codec[1])) result.push(`a=fmtp:${codec[1]} ${hints}`);
    }
  }
  return { type: description.type, sdp: result.join('\r\n') };
}

export async function timedFetch(path, options, timeout = 4500) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeout);
  try {
    return await fetch(path, { ...options, signal: controller.signal });
  } finally {
    clearTimeout(timer);
  }
}

export async function gather(peer, description) {
  await peer.setLocalDescription(description);
  if (peer.iceGatheringState !== 'complete') {
    await new Promise((resolve) => {
      const complete = () => {
        clearTimeout(timeout);
        peer.removeEventListener('icegatheringstatechange', update);
        resolve();
      };
      const update = () => {
        if (peer.iceGatheringState === 'complete' || peer.signalingState === 'closed') complete();
      };
      const timeout = setTimeout(complete, 2500);
      peer.addEventListener('icegatheringstatechange', update);
      update();
    });
  }
  return peer.localDescription?.sdp || '';
}
