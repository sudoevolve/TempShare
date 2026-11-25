const preference = 'tempshare-appearance';
const choices = ['system', 'light', 'dark'];
const system = matchMedia('(prefers-color-scheme: dark)');
const select = document.getElementById('appearance');
let appearance = 'system';
try {
  const saved = localStorage.getItem(preference);
  if (choices.includes(saved)) appearance = saved;
} catch { /* 仍允许在当前页面切换。 */ }

function updateAppearance() {
  const mode = appearance === 'system' ? (system.matches ? 'dark' : 'light') : appearance;
  document.documentElement.dataset.appearance = mode;
  document.querySelector('meta[name="theme-color"]')?.setAttribute('content', mode === 'dark' ? '#101427' : '#eff2fc');
  select.value = appearance;
}

select.addEventListener('change', () => {
  appearance = select.value;
  updateAppearance();
  try { localStorage.setItem(preference, appearance); } catch { /* 当前页面已更新。 */ }
});
window.addEventListener('storage', (event) => {
  if (event.key !== preference) return;
  appearance = choices.includes(event.newValue) ? event.newValue : 'system';
  updateAppearance();
});
if (system.addEventListener) system.addEventListener('change', updateAppearance);
else system.addListener(updateAppearance);
updateAppearance();
