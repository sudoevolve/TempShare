const storageKey = 'tempshare-upload-resume-v1';
const blockSize = 4 * 1024 ** 2;
let database;

function uploadHandles() {
  if (!database) database = new Promise((resolve, reject) => {
    const open = indexedDB.open('TempShare-upload-handles', 1);
    open.onupgradeneeded = () => open.result.createObjectStore('handles');
    open.onsuccess = () => resolve(open.result);
    open.onerror = () => reject(open.error);
  });
  return database;
}

export async function savedHandle(id, handle) {
  try {
    const db = await uploadHandles();
    return await new Promise((resolve, reject) => {
      const transaction = db.transaction('handles', handle ? 'readwrite' : 'readonly');
      const store = transaction.objectStore('handles');
      const request = handle ? store.put(handle, id) : store.get(id);
      transaction.oncomplete = () => resolve(request.result);
      transaction.onabort = () => reject(transaction.error);
    });
  } catch {
    return undefined;
  }
}

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
  const discarded = id ? [id] : resumableUploads().map((entry) => entry.id);
  try {
    if (id) sessionStorage.setItem(storageKey, JSON.stringify(resumableUploads().filter((entry) => entry.id !== id)));
    else sessionStorage.removeItem(storageKey);
  } catch { /* Private browsing or disabled storage. */ }
  if (discarded.length && typeof window.showOpenFilePicker === 'function') {
    uploadHandles().then((db) => {
      const transaction = db.transaction('handles', 'readwrite');
      const store = transaction.objectStore('handles');
      for (const entry of discarded) store.delete(entry);
    }).catch(() => {});
  }
}
