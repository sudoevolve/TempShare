export function peerConnection() {
  return new RTCPeerConnection({ iceServers: [], bundlePolicy: 'max-bundle' });
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
      const timeout = setTimeout(complete, 4200);
      peer.addEventListener('icegatheringstatechange', update);
      update();
    });
  }
  return peer.localDescription?.sdp || '';
}
