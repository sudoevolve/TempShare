const storageKey = 'tempshare-upload-resume-v1';
const blockSize = 4 * 1024 ** 2;

// Hash fixed-size blocks first so a large file can be verified without reading it all into memory.
export async function fingerprint(file, cancelled) {
  const hashes = new Uint8Array(Math.ceil(file.size / blockSize) * 32);
  for (let offset = 0; offset < file.size; offset += blockSize) {
    if (cancelled()) return '';
    const bytes = await file.slice(offset, offset + blockSize).arrayBuffer();
    const hash = await crypto.subtle.digest('SHA-256', bytes);
    hashes.set(new Uint8Array(hash), offset / blockSize * 32);
  }
  const digest = await crypto.subtle.digest('SHA-256', hashes);
  return Array.from(new Uint8Array(digest), (byte) => byte.toString(16).padStart(2, '0')).join('');
}

export function resumableUploads() {
  try {
    const values = JSON.parse(sessionStorage.getItem(storageKey) || '[]');
    return Array.isArray(values) ? values.filter((entry) => entry && /^[a-f0-9]{32}$/.test(entry.id) &&
      /^[a-f0-9]{64}$/.test(entry.digest) && typeof entry.name === 'string' && Number.isSafeInteger(entry.size)) : [];
  } catch {
    return [];
  }
}

export function rememberUpload(entry) {
  const records = resumableUploads().filter(({ id }) => id !== entry.id);
  records.push(entry);
  try {
    sessionStorage.setItem(storageKey, JSON.stringify(records.slice(-100)));
    return true;
  } catch {
    return false;
  }
}

export function forgetUpload(id) {
  try {
    if (id) sessionStorage.setItem(storageKey, JSON.stringify(resumableUploads().filter((entry) => entry.id !== id)));
    else sessionStorage.removeItem(storageKey);
  } catch { /* Private browsing or disabled storage. */ }
}
