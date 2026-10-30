export async function api(path, data) {
  const response = await fetch(path, data === undefined ? {} : {
    method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(data)
  });
  const value = await response.json();
  if (!response.ok) throw new Error(value.errors ? Object.entries(value.errors).map(([k,v]) => `${k}: ${v.join(', ')}`).join('\n') : value.error || `HTTP ${response.status}`);
  return value;
}
export function node(tag, text, className) {
  const element = document.createElement(tag);
  if (text !== undefined) element.textContent = text;
  if (className) element.className = className;
  return element;
}
export function report(error) {
  const message = document.querySelector('#message');
  message.textContent = error.message || error;
  message.className = error instanceof Error ? 'error' : '';
}
export function formData(form) { return Object.fromEntries(new FormData(form)); }
export async function action(callback) {
  try { report(''); await callback(); } catch (error) { report(error); }
}
