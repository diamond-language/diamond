'use strict';
// Show page for one cut: /cuts/<name>. Data comes from catalog/<name>.json.
const base = new URL('..', location.href);
const name = decodeURIComponent(location.pathname.split('/').filter(Boolean).pop() || '');
const status = document.querySelector('#status');

function node(tag, text, className) {
  const element = document.createElement(tag);
  if (text) element.textContent = text;
  if (className) element.className = className;
  return element;
}
function safeHref(url) {
  try {
    const parsed = new URL(url, base);
    return ['https:', 'mailto:'].includes(parsed.protocol) ? parsed.href : null;
  } catch {
    return null;
  }
}
function link(text, url) {
  const href = safeHref(url);
  if (!href) return document.createTextNode(text);
  const anchor = node('a', text);
  anchor.href = href;
  anchor.rel = 'nofollow noopener';
  return anchor;
}
function formatSize(bytes) {
  const units = ['bytes', 'KB', 'MB', 'GB'];
  let value = bytes, unit = 0;
  while (value >= 1024 && unit < units.length - 1) { value /= 1024; unit++; }
  return unit === 0 ? `${value} bytes` : `${value.toFixed(value < 10 ? 1 : 0)} ${units[unit]}`;
}
function formatDate(seconds) {
  return new Date(seconds * 1000).toLocaleDateString(undefined, {year: 'numeric', month: 'short', day: 'numeric'});
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

// A small Markdown subset rendered straight to DOM nodes (never innerHTML):
// headings, paragraphs, fenced code, lists, blockquotes, rules, and inline
// code, emphasis, and links. Only https and mailto links become anchors.
const INLINE = /(`+)([\s\S]*?[^`])\1(?!`)|\*\*([^*]+)\*\*|__([^_]+)__|\*([^*\s][^*]*)\*|(?<![\w])_([^_\s][^_]*)_(?![\w])|!?\[([^\]]*)\]\(([^)\s]+)(?:\s+"[^"]*")?\)|<((?:https:\/\/|mailto:)[^>\s]+)>|(https:\/\/[^\s<>()]+[^\s<>().,;:!?'"])/g;
function inline(text, parent) {
  let last = 0;
  for (const match of text.matchAll(INLINE)) {
    if (match.index > last) parent.append(text.slice(last, match.index));
    const [whole, , code, strong1, strong2, em1, em2, label, url, auto, bare] = match;
    if (code !== undefined) parent.append(node('code', code.trim()));
    else if (strong1 || strong2) inline(strong1 || strong2, parent.appendChild(node('strong')));
    else if (em1 || em2) inline(em1 || em2, parent.appendChild(node('em')));
    else if (url !== undefined) {
      const href = safeHref(url);
      if (whole.startsWith('!') || !href) parent.append(label || url);
      else { const anchor = link('', url); inline(label || url, anchor); parent.append(anchor); }
    } else parent.append(link(auto || bare, auto || bare));
    last = match.index + whole.length;
  }
  if (last < text.length) parent.append(text.slice(last));
}
function markdown(source, root) {
  const lines = source.replace(/\r\n?/g, '\n').split('\n');
  let i = 0;
  const isBlockStart = line => /^(#{1,6}\s|```|~~~|>\s?|\s*([-*+]|\d+[.)])\s+|\s*([-*_])(\s*\3){2,}\s*$)/.test(line);
  while (i < lines.length) {
    const line = lines[i];
    if (!line.trim()) { i++; continue; }
    let match;
    if ((match = line.match(/^(```|~~~)/))) {
      const fence = match[1], body = [];
      i++;
      while (i < lines.length && !lines[i].startsWith(fence)) body.push(lines[i++]);
      i++;
      const pre = node('pre');
      pre.append(node('code', body.join('\n')));
      root.append(pre);
    } else if ((match = line.match(/^(#{1,6})\s+(.*?)\s*#*\s*$/))) {
      inline(match[2], root.appendChild(node('h' + match[1].length)));
      i++;
    } else if (/^\s*([-*_])(\s*\1){2,}\s*$/.test(line)) {
      root.append(node('hr'));
      i++;
    } else if (/^>\s?/.test(line)) {
      const body = [];
      while (i < lines.length && /^>\s?/.test(lines[i])) body.push(lines[i++].replace(/^>\s?/, ''));
      markdown(body.join('\n'), root.appendChild(node('blockquote')));
    } else if ((match = line.match(/^(\s*)([-*+]|\d+[.)])\s+/))) {
      const ordered = /\d/.test(match[2]);
      const list = root.appendChild(node(ordered ? 'ol' : 'ul'));
      let item = null;
      while (i < lines.length) {
        const current = lines[i];
        const bullet = current.match(/^\s*([-*+]|\d+[.)])\s+(.*)$/);
        if (bullet) { item = list.appendChild(node('li')); inline(bullet[2], item); i++; }
        else if (current.trim() && /^\s{2,}/.test(current) && item) { item.append(' '); inline(current.trim(), item); i++; }
        else break;
      }
    } else {
      const body = [];
      while (i < lines.length && lines[i].trim() && !(body.length && isBlockStart(lines[i]))) body.push(lines[i++].trim());
      inline(body.join(' '), root.appendChild(node('p')));
    }
  }
}

function field(parent, label, value) {
  const paragraph = parent.appendChild(node('p'));
  paragraph.append(node('span', label + ' ', 'label'));
  if (Array.isArray(value)) paragraph.append(...value); else paragraph.append(value);
}
function render(cut) {
  document.title = `${cut.name} · Diamond cuts`;
  document.querySelector('#crumb-name').textContent = cut.name;
  document.querySelector('#cut-name').textContent = cut.name;
  const latest = cut.versions.find(version => version.version === cut.latest);
  const badge = document.querySelector('#cut-version');
  badge.textContent = cut.latest;
  badge.hidden = false;
  document.querySelector('#cut-summary').textContent = cut.summary || '';

  const readme = document.querySelector('#readme');
  if (cut.readme) markdown(cut.readme, readme);
  else readme.append(node('p', 'This release has no readable README.', 'empty'));

  const install = document.querySelector('#install');
  const registry = base.href.replace(/\/$/, '');
  if (latest.yanked) {
    install.append(node('p', 'Every release is yanked; existing facet.lock files can still install them.', 'empty'));
  } else {
    const command = `facet add ${cut.name} --registry ${registry} --version '^${cut.latest}'`;
    const box = node('div', '', 'command');
    const bar = node('div', '', 'command-bar');
    bar.append(node('span', 'Add to diamond.cut'), copyButton(() => command));
    box.append(bar, node('pre', `facet add ${cut.name} \\\n  --registry ${registry} \\\n  --version '^${cut.latest}'`));
    install.append(box);
  }

  const about = document.querySelector('#about');
  if (cut.license) field(about, 'License', cut.license);
  const people = [];
  latest.maintainers.forEach(({name: person, contact}, index) => {
    if (index) people.push(', ');
    people.push(link(person, contact.startsWith('https://') ? contact : 'mailto:' + contact));
  });
  field(about, 'Maintainers', people.length ? people : 'not recorded');
  const dependencies = Object.entries(latest.dependencies).map(([dependency, range], index) => {
    const anchor = node('a', `${dependency} ${range}`);
    anchor.href = new URL(`cuts/${encodeURIComponent(dependency)}`, base).href;
    return index ? [', ', anchor] : [anchor];
  }).flat();
  field(about, 'Dependencies', dependencies.length ? dependencies : 'None');
  ['homepage', 'source', 'documentation', 'issues'].forEach(key => {
    if (cut[key] && safeHref(cut[key])) field(about, key[0].toUpperCase() + key.slice(1), link(cut[key].replace(/^https:\/\//, ''), cut[key]));
  });
  field(about, 'Size', formatSize(latest.size));
  about.append(node('p', 'sha256 ' + latest.sha256, 'digest'));

  const versions = document.querySelector('#versions');
  cut.versions.forEach(version => {
    const item = node('li');
    const label = node('span', '', 'v');
    const metadata = node('a', version.version);
    metadata.href = new URL(`v1/cuts/${encodeURIComponent(cut.name)}/versions/${encodeURIComponent(version.version)}`, base).href;
    metadata.title = 'Release metadata';
    label.append(metadata);
    if (version.version === cut.latest) label.append(node('span', 'latest', 'tag latest'));
    if (version.yanked) label.append(node('span', 'yanked', 'tag yanked-tag'));
    item.append(label, node('span', formatDate(version.published_at), 'when'));
    versions.append(item);
  });
  status.textContent = '';
  document.querySelector('#cut').hidden = false;
}

async function load() {
  document.querySelector('#cut-name').textContent = name;
  document.querySelector('#crumb-name').textContent = name;
  status.textContent = 'Loading…';
  try {
    const response = await fetch(new URL(`catalog/${encodeURIComponent(name)}.json`, base));
    if (response.status === 404) {
      status.textContent = 'No published cut has this name.';
      return;
    }
    if (!response.ok) throw new Error('request failed');
    render(await response.json());
  } catch {
    status.textContent = 'Could not load this cut. Please retry.';
  }
}
load();
