'use strict';
// Markdown subset shared by the catalog (card previews) and the show page.
// Renders straight to DOM nodes, never innerHTML. Pages define node(),
// safeHref() and link() before calling markdown().
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
