export function peerConnection() {
  return new RTCPeerConnection({ iceServers: [], bundlePolicy: 'max-bundle' });
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
