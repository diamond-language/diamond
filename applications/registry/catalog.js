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
function copyButton(getText) {
  const button = node('button', 'Copy', 'copy-btn');
  button.type = 'button';
  button.addEventListener('click', async () => {
    try {
      await navigator.clipboard.writeText(getText());
      button.textContent = 'Copied';
      button.classList.add('copied');
    } catch {
      button.textContent = 'Select to copy';
    }
    setTimeout(() => { button.textContent = 'Copy'; button.classList.remove('copied'); }, 1600);
  });
  return button;
}
function field(label, value) {
  const paragraph = node('p', '');
  paragraph.append(node('span', label + ' ', 'label'));
  if (typeof value === 'string') paragraph.append(value);
  else paragraph.append(...value);
  return paragraph;
}
function render() {
  const query = document.querySelector('#search').value.trim().toLowerCase();
  const visible = releases.filter(release => release.name.toLowerCase().includes(query));
  const cards = visible.map(release => {
    const card = node('article', '', 'release-card');
    const head = node('div', '', 'release-head');
    head.append(node('h3', release.name), node('span', release.version, 'version'));
    card.append(head);
    if (release.yanked) card.append(node('span', 'Yanked', 'yanked'));
    const dependencies = Object.entries(JSON.parse(release.dependencies)).map(([name, range]) => name + ' ' + range).join(', ');
    card.append(field('Dependencies', dependencies || 'None'));
    const maintainers = JSON.parse(release.maintainers || '[]');
    const people = [];
    maintainers.forEach(({name, contact}, index) => {
      if (index) people.push(', ');
      const link = node('a', name);
      link.href = contact.startsWith('https://') ? contact : 'mailto:' + contact;
      link.rel = 'nofollow noopener';
      people.push(link);
    });
    card.append(field('Maintainers', people.length ? people : 'not recorded'));
    const command = `facet add ${release.name} --registry ${base.href.replace(/\/$/, '')} --version '${release.version}'`;
    const box = node('div', '', 'command');
    const display = `facet add ${release.name} \\\n  --registry ${base.href.replace(/\/$/, '')} \\\n  --version '${release.version}'`;
    const text = release.yanked ? 'Available through an existing facet.lock only.' : display;
    if (!release.yanked) {
      const bar = node('div', '', 'command-bar');
      bar.append(node('span', 'Install'), copyButton(() => command));
      box.append(bar);
    }
    box.append(node('pre', text));
    card.append(box);
    card.append(node('p', `${release.size.toLocaleString()} bytes`), node('p', 'sha256 ' + release.sha256, 'digest'));
    const link = node('a', 'Release metadata →');
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
document.querySelectorAll('[data-copy-target]').forEach(button => {
  const target = document.getElementById(button.dataset.copyTarget);
  const copy = copyButton(() => target.innerText.trim());
  copy.dataset.copyTarget = button.dataset.copyTarget;
  button.replaceWith(copy);
});
more.addEventListener('click', load);
load();
