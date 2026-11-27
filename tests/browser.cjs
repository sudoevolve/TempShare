const { chromium } = require(process.env.TEMPSHARE_PLAYWRIGHT || 'playwright-core');
const { spawn } = require('node:child_process');
const { createInterface } = require('node:readline');
const assert = require('node:assert/strict');
const fixture = spawn(process.argv[2], [], { stdio: ['pipe', 'pipe', 'inherit'] });
const replies = [];
const waiters = [];
let fixtureError;
function failFixture(error) {
  fixtureError = error;
  for (const waiter of waiters.splice(0)) { clearTimeout(waiter.timer); waiter.reject(error); }
}
fixture.on('error', failFixture);
fixture.on('exit', code => failFixture(Error('Browser fixture exited with code ' + code)));
createInterface({ input: fixture.stdout }).on('line', line => {
  try {
    const value = JSON.parse(line);
    if (waiters.length) {
      const waiter = waiters.shift(); clearTimeout(waiter.timer); waiter.resolve(value);
    } else replies.push(value);
  } catch (error) { failFixture(error); }
});
function reply() {
  if (replies.length) return Promise.resolve(replies.shift());
  if (fixtureError) return Promise.reject(fixtureError);
  return new Promise((resolve, reject) => {
    const waiter = { resolve, reject, timer: setTimeout(() => failFixture(Error('Browser fixture response timed out')), 15000) };
    waiters.push(waiter);
  });
}
async function command(text) { fixture.stdin.write(text + '\n'); return reply(); }
async function until(predicate, description) {
  const end = Date.now() + 12000;
  while (Date.now() < end) { if (await predicate()) return; await new Promise(resolve => setTimeout(resolve, 100)); }
  throw Error(description);
}
(async () => {
  let browser;
  try {
    const ports = await reply();
    browser = await chromium.launch({ executablePath: process.env.TEMPSHARE_BROWSER, headless: true, args: ['--no-sandbox'] });
    const context = await browser.newContext({ acceptDownloads: true });
    const errors = [];
    context.on('page', page => page.on('pageerror', error => errors.push(error.message)));
    const landing = await context.newPage();
    await landing.goto('http://127.0.0.1:' + ports.entry);
    assert(landing.url().includes('/s/2/'), 'web guests use the available session route');
    await command('join');
    await until(async () => { await command('approve'); return (await command('status')).approved; }, 'native pairing completes');
    const state = await command('capture');
    const host = await context.newPage();
    await host.addInitScript(() => {
      const prototype = RTCRtpSender.prototype;
      const original = prototype.setParameters;
      window.testSenderParameters = [];
      prototype.setParameters = function(parameters) {
        window.testSenderParameters.push(JSON.parse(JSON.stringify(parameters)));
        return original.call(this, parameters);
      };
      navigator.mediaDevices.getDisplayMedia = async () => {
        const canvas = document.createElement('canvas'); canvas.width = 1920; canvas.height = 1080;
        const g = canvas.getContext('2d'); let x = 0;
        setInterval(() => { g.fillStyle = x++ % 2 ? '#216bce' : '#28c891'; g.fillRect(0, 0, 1920, 1080); }, 1000 / 60);
        return canvas.captureStream(60);
      };
    });
    await host.goto('http://127.0.0.1:' + ports.entry + '/s/2/cast#' + state.hostKey);
    const rtc = await host.evaluate(async () => {
      const { screenBitrate, screenOffer } = await import('/rtc.js');
      const before = 'v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=rtpmap:111 opus/48000/2\r\nm=video 9 UDP/TLS/RTP/SAVPF 96 97 98\r\na=rtpmap:96 VP8/90000\r\na=rtpmap:97 rtx/90000\r\na=fmtp:97 apt=96\r\na=rtpmap:98 H264/90000\r\na=fmtp:98 profile-level-id=42e01f;packetization-mode=1\r\n';
      return { budget: screenBitrate(1920, 1080, 60), manual: screenBitrate(1920, 1080, 60, 64), offer: screenOffer({ type: 'offer', sdp: before }, 32000000).sdp };
    });
    assert(rtc.budget > 20000000 && rtc.manual === 64000000, 'screen bitrate respects capture demand and manual choice');
    assert(rtc.offer.includes('a=fmtp:96 x-google-start-bitrate=20800;x-google-max-bitrate=32000') &&
      rtc.offer.includes('profile-level-id=42e01f;packetization-mode=1;x-google-start-bitrate=20800') &&
      rtc.offer.includes('a=fmtp:97 apt=96\r\n'), 'startup bitrate preserves codec and RTX parameters');
    await host.locator('#start').click();
    const viewer = await context.newPage();
    await viewer.goto('http://127.0.0.1:' + ports.guest + '/peer#' + state.guestPeerKey + ':cast');
    await viewer.locator('#cast-watch').waitFor({ state: 'visible' });
    await until(() => viewer.locator('#cast-watch').isEnabled(), 'native viewer sees host readiness');
    await viewer.locator('#cast-watch').click();
    await until(() => viewer.locator('#cast-status').textContent().then(text => text.includes('正在播放')),
      'WebRTC direct playback receives actual video frames');
    await until(() => host.locator('#transport-info').textContent().then(text => text.includes('实际发送')), 'actual sending stats are visible');
    const playback = await viewer.locator('#cast-video').evaluate(video => ({ width: video.videoWidth, height: video.videoHeight }));
    assert(playback.width === 1920 && playback.height === 1080, 'direct playback reaches requested source resolution');
    const encoding = await host.evaluate(() => window.testSenderParameters.at(-1)?.encodings[0]);
    assert(encoding?.maxFramerate === 60 && encoding.scaleResolutionDownBy === 1 && encoding.maxBitrate > 20000000,
      'sender honors requested frame and resolution limits without accumulating extra degradation');
    console.log('Direct media sample: ' + await host.locator('#transport-info').textContent());
    await host.locator('#rate').selectOption('15');
    await until(() => host.evaluate(() => window.testSenderParameters.at(-1)?.encodings[0].maxFramerate === 15), 'lower frame target applies to current sender');
    await host.locator('#rate').selectOption('60');
    await host.locator('#bitrate').selectOption('32');
    await until(() => host.evaluate(() => {
      const encoding = window.testSenderParameters.at(-1)?.encodings[0];
      return encoding?.maxFramerate === 60 && encoding.maxBitrate === 32000000;
    }), 'restoring frame target and setting bitrate raises current sender limits again');
    await viewer.locator('#cast-mode').click();
    await until(() => viewer.locator('#cast-status').textContent().then(text => text.includes('兼容播放') && text.includes('fps')),
      'real canvas frames reach the viewer through the native bridge');
    assert((await host.locator('#status').textContent()).includes('兼容播放'), 'sender reports the actual transport mode');
    assert.equal(await viewer.locator('#cast-frames').evaluate(canvas => canvas.width), 1920, 'compatibility frames retain 1080p instead of forcing 720p');
    console.log('Compatibility media sample: ' + await viewer.locator('#cast-status').textContent());
    await viewer.getByRole('button', { name: '文件分享', exact: true }).click();
    const downloaded = viewer.waitForEvent('download');
    await viewer.getByRole('link', { name: '下载 fixture.txt', exact: true }).click();
    assert.equal((await downloaded).suggestedFilename(), 'fixture.txt');
    await viewer.locator('#file-input').setInputFiles({ name: 'browser-send.txt', mimeType: 'text/plain', buffer: Buffer.from('browser to desktop') });
    await until(async () => { await command('approve-files'); return (await command('status')).received === 1; },
      'browser file upload through peer gateway is explicitly approved and saved');
    await viewer.getByRole('button', { name: /内容同步/ }).click();
    await viewer.locator('#sync-text').fill('browser message');
    await viewer.locator('#sync-form button').click();
    await until(async () => (await command('status')).hostMessages >= 1, 'browser text reaches original host');
    await command('message');
    await viewer.getByText('native desktop message', { exact: true }).waitFor();
    await command('unlimited');
    await until(() => viewer.locator('#session-deadline').textContent().then(text => text.includes('不限时')), 'unlimited session status reaches viewer');
    assert(await viewer.locator('#extend-session').isHidden(), 'unlimited sessions do not offer redundant renewal');
    await command('finite');
    await until(() => viewer.locator('#session-deadline').textContent().then(text => text.includes('120 分钟')), 'finite duration updates without losing connection');
    await command('permissions');
    await viewer.locator('#sync-disabled').waitFor({ state: 'visible' });
    await command('permissions');
    await viewer.getByText('native desktop message', { exact: true }).waitFor();
    for (const width of [320, 390, 1024]) {
      await viewer.setViewportSize({ width, height: 850 });
      assert(await viewer.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'no horizontal overflow at ' + width);
    }
    await host.reload();
    await host.locator('#start').waitFor({ state: 'visible' });
    assert(await host.locator('#start').isEnabled(), 'local capture console can be recovered after refresh');
    const reverse = await command('reverse');
    await host.addInitScript(() => Object.defineProperty(window, 'MediaStreamTrackProcessor', { value: undefined, configurable: true }));
    await host.goto('http://127.0.0.1:' + ports.guest + '/cast#' + reverse.guestKey);
    await host.locator('#start').click();
    await viewer.goto('http://127.0.0.1:' + ports.entry + '/s/2/peer#' + reverse.hostPeerKey + ':cast');
    await until(() => viewer.locator('#cast-watch').isEnabled(), 'original host can watch the joining computer');
    await viewer.locator('#cast-watch').click();
    await viewer.locator('#cast-mode').click();
    await until(() => viewer.locator('#cast-status').textContent().then(text => text.includes('兼容播放') && text.includes('fps')),
      'reverse screen frames also reach the original host');
    assert.equal(await host.evaluate(() => typeof MediaStreamTrackProcessor), 'undefined', 'reverse playback covers Worker fallback without track processor');
    assert.deepEqual(errors, [], 'browser application has no unhandled JavaScript errors');
    console.log('Browser roundtrip passed: gateway, native pairing, screen frames in both directions, files, messages, permission recovery, mobile layout, refresh.');
  } finally {
    if (browser) await browser.close();
    if (!fixtureError) fixture.stdin.write('quit\n');
    fixture.stdin.end();
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
