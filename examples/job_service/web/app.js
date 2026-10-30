import {api,node,report,formData,action} from '/common.js';
const $ = s => document.querySelector(s); let loading = false;
function renderJobs(rows) {
  const list = $('#jobs'); list.replaceChildren();
  for (const job of rows) {
    const card = node('article',undefined,'job'), heading = node('div',undefined,'job-heading');
    heading.append(node('strong',`#${job.id} · ${job.kind === 'import_csv' ? 'Contact import' : 'Count demo'}`),node('span',job.status,`badge status-${job.status}`)); card.append(heading);
    const progress = node('progress'); progress.max = job.total || 1; progress.value = job.progress; progress.setAttribute('aria-label',`Job ${job.id} progress`); card.append(progress);
    card.append(node('small',`${job.progress} / ${job.total} rows checked · ${job.attempts} failed attempts`));
    if (job.last_error) card.append(node('p',job.last_error));
    if (job.summary) {
      const summary = JSON.parse(job.summary);
      if (summary.imported !== undefined) card.append(node('p',`${summary.imported} contacts imported.`));
      if (summary.errors) { const errors = node('ul',undefined,'errors'); for (const error of summary.errors) errors.append(node('li',`Row ${error.row}: ${error.error}`)); card.append(errors); }
    }
    const controls = node('div',undefined,'actions');
    if (['pending','running','failed','cancelled','timed_out'].includes(job.status)) {
      const operation = ['pending','running'].includes(job.status) ? 'cancel' : 'retry', button = node('button',operation === 'cancel' ? 'Cancel job' : 'Retry job');
      button.onclick = () => action(async () => { button.disabled = true; await api(`/jobs/${job.id}/${operation}`,{}); await refresh(); }); controls.append(button);
    }
    card.append(controls); list.append(card);
  }
  if (!rows.length) list.append(node('p','No jobs yet. Queue the sample import to begin.','empty'));
}
async function refresh() {
  if (loading) return; loading = true;
  try {
    const [jobs,contacts] = await Promise.all([api('/jobs'),api('/contacts')]); renderJobs(jobs); const table = $('#contacts'); table.replaceChildren();
    for (const contact of contacts) { const row = node('tr'); row.append(node('td',contact.name),node('td',contact.email)); table.append(row); }
  } finally { loading = false; }
}
$('#file').onchange = () => action(async () => { const file = $('#file').files[0]; if (!file) return; if (file.size > 65536) throw new Error('Choose a CSV file up to 64 KiB.'); $('#import').elements.csv.value = await file.text(); });
$('#import').onsubmit = event => { event.preventDefault(); action(async () => {
  const data = formData(event.target); data.timeout_ms = Number(data.timeout_ms); const job = await api('/jobs',data); report(`Queued import #${job.id}.`); await refresh();
}); };
action(refresh); setInterval(() => action(refresh),1000);
