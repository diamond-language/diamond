import {api,node,report,formData,action} from '/common.js';
const $ = s => document.querySelector(s); let rows = [];
const tags = row => row.tags.split(',').map(t => t.trim()).filter(Boolean);
function render() {
  const list = $('#bookmarks'), needle = $('#search').value.toLowerCase(), tag = $('#tag').value; list.replaceChildren();
  const found = rows.filter(r => [r.title,r.url,r.tags].join(' ').toLowerCase().includes(needle) && (!tag || tags(r).includes(tag)));
  $('#count').textContent = `${found.length} of ${rows.length} bookmarks`;
  for (const row of found) {
    const card = node('article',undefined,'card'), heading = node('h2'), link = node('a',row.title);
    link.href = row.url; link.target = '_blank'; link.rel = 'noopener noreferrer'; heading.append(link); card.append(heading,node('small',row.url));
    for (const tag of tags(row)) { const button = node('button',tag,'badge'); button.onclick = () => { $('#tag').value = tag; render(); }; card.append(button); }
    list.append(card);
  }
  if (!found.length) list.append(node('p',rows.length ? 'No matching bookmarks.' : 'Your shelf is empty. Add your first link.','empty'));
}
async function refresh() {
  rows = await api('/api/bookmarks'); const select = $('#tag'), old = select.value; select.replaceChildren();
  const all = node('option','All tags'); all.value = ''; select.append(all);
  for (const tag of [...new Set(rows.flatMap(tags))].sort()) { const option = node('option',tag); option.value = tag; select.append(option); }
  if ([...select.options].some(o => o.value === old)) select.value = old; render();
}
$('#add').onsubmit = event => { event.preventDefault(); action(async () => { await api('/api/bookmarks',formData(event.target)); event.target.reset(); await refresh(); report('Bookmark saved.'); }); };
$('#import').onsubmit = event => { event.preventDefault(); action(async () => {
  const text = formData(event.target).text.trim(), data = text.startsWith('[') ? {bookmarks:JSON.parse(text)} : {text};
  const result = await api('/api/import',data); await refresh(); report(`${result.added} added, ${result.refused} refused.`);
}); };
$('#export').onclick = () => action(async () => {
  const blob = new Blob([JSON.stringify(await api('/api/export'),null,2)],{type:'application/json'}), url = URL.createObjectURL(blob), a = node('a');
  a.href = url; a.download = 'bookmarks.json'; a.click(); setTimeout(() => URL.revokeObjectURL(url),1000);
});
$('#search').oninput = render; $('#tag').onchange = render; $('#refresh').onclick = () => action(refresh); action(refresh);
