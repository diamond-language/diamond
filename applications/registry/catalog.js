'use strict';
const releases = [];
let cursor = null;
const status = document.querySelector('#status');
const more = document.querySelector('#more');
const base = new URL('.', location.href);
function node(tag, text, className) {
  const element = document.createElement(tag);
  element.textContent = text;
  if (className) element.className = className;
  return element;
}
function render() {
  const query = document.querySelector('#search').value.trim().toLowerCase();
  const visible = releases.filter(release => release.name.toLowerCase().includes(query));
  const cards = visible.map(release => {
    const card = node('article', '');
    card.append(node('h3', release.name), node('p', release.version + (release.yanked ? ' · Yanked' : ''), release.yanked ? 'status' : 'muted'));
    const dependencies = Object.entries(JSON.parse(release.dependencies)).map(([name, range]) => name + ' ' + range).join(', ');
    card.append(node('p', 'Dependencies: ' + (dependencies || 'None')));
    const maintainers = JSON.parse(release.maintainers || '[]');
    const maintained = node('p', maintainers.length ? 'Maintainers: ' : 'Maintainers: not recorded');
    maintainers.forEach(({name, contact}, index) => {
      if (index) maintained.append(', ');
      const link = node('a', name);
      link.href = contact.startsWith('https://') ? contact : 'mailto:' + contact;
      link.rel = 'nofollow noopener';
      maintained.append(link);
    });
    card.append(maintained);
    const command = `facet add ${release.name} --registry ${base.href.replace(/\/$/, '')} --version '${release.version}'`;
    card.append(node('pre', release.yanked ? 'Available through an existing facet.lock only.' : command), node('p', `${release.size.toLocaleString()} bytes`), node('p', 'SHA-256: ' + release.sha256, 'digest'));
    const link = node('a', 'Release metadata');
    link.href = new URL(`v1/cuts/${encodeURIComponent(release.name)}/versions/${encodeURIComponent(release.version)}`, base);
    card.append(link);
    return card;
  });
  document.querySelector('#releases').replaceChildren(...cards);
  status.textContent = releases.length ? `${visible.length} matching releases of ${releases.length} loaded.` : 'No releases have been published yet.';
}
async function load() {
  more.disabled = true;
  try {
    const response = await fetch(new URL('catalog.json' + (cursor === null ? '' : '?after=' + cursor), base));
    if (!response.ok) throw new Error('request failed');
    const page = await response.json();
    releases.push(...page.releases);
    cursor = page.next_after;
    more.hidden = cursor === null;
    more.textContent = 'Load more releases';
    render();
  } catch {
    status.textContent = 'Could not load releases. Please retry.';
    more.hidden = false;
    more.textContent = 'Retry loading releases';
  } finally {
    more.disabled = false;
  }
}
document.querySelector('#search').addEventListener('input', render);
more.addEventListener('click', load);
load();
