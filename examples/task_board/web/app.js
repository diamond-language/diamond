import {api,node,report,formData,action} from '/common.js';
const $ = s => document.querySelector(s), form = $('#task-form'), dialog = $('#editor');
let state = {projects:[],tasks:[]};
function options(select, all = false) {
  const old = select.value; select.replaceChildren();
  if (all) { const option = node('option','All projects'); option.value = ''; select.append(option); }
  for (const p of state.projects) { const option = node('option',p.name); option.value = p.id; select.append(option); }
  if ([...select.options].some(o => o.value === old)) select.value = old;
}
function render() {
  const board = $('#board'); board.replaceChildren();
  const needle = $('#search').value.toLowerCase(), project = $('#filter').value;
  for (const [status,label] of [['todo','To do'],['doing','In progress'],['done','Done']]) {
    const tasks = state.tasks.filter(t => t.status === status && (!project || String(t.project_id) === project) && [t.title,t.notes,t.project].some(v => v.toLowerCase().includes(needle)));
    const column = node('section',undefined,'column'), heading = node('h2',label);
    heading.append(node('span',String(tasks.length),'badge')); column.append(heading);
    for (const task of tasks) {
      const card = node('article',undefined,'card'); card.append(node('small',task.project),node('h3',task.title));
      if (task.notes) card.append(node('p',task.notes));
      const actions = node('div',undefined,'actions'), edit = node('button','Edit'), select = node('select'); select.setAttribute('aria-label',`Status of ${task.title}`);
      for (const [value,text] of [['todo','To do'],['doing','In progress'],['done','Done']]) { const o = node('option',text); o.value = value; select.append(o); }
      select.value = task.status; select.onchange = () => action(async () => { await api(`/api/tasks/${task.id}`,{status:select.value}); await refresh(); });
      edit.onclick = () => open(task); actions.append(select,edit); card.append(actions); column.append(card);
    }
    if (!tasks.length) column.append(node('p',state.projects.length ? 'No tasks here yet.' : 'Create a project above to add your first task.','empty'));
    board.append(column);
  }
  $('#new-task').disabled = !state.projects.length;
}
async function refresh() { state = await api('/api/board'); options($('#filter'),true); options(form.elements.project_id); render(); }
function open(task = {id:'',title:'',notes:'',status:'todo',project_id:$('#filter').value || state.projects[0]?.id}) {
  for (const [key,value] of Object.entries(task)) if (form.elements.namedItem(key)) form.elements.namedItem(key).value = value;
  $('#editor-title').textContent = task.id ? 'Edit task' : 'New task'; $('#form-error').textContent = ''; dialog.showModal();
}
$('#new-task').onclick = () => open(); $('#close').onclick = () => dialog.close();
$('#filter').onchange = render; $('#search').oninput = render; $('#refresh').onclick = () => action(refresh);
$('#project-form').onsubmit = event => { event.preventDefault(); action(async () => { await api('/api/projects',formData(event.target)); event.target.reset(); await refresh(); }); };
form.onsubmit = async event => {
  event.preventDefault(); const data = formData(form), id = data.id; delete data.id; data.project_id = Number(data.project_id);
  try { await api(id ? `/api/tasks/${id}` : '/api/tasks',data); dialog.close(); await refresh(); report('Task saved.'); } catch (error) { $('#form-error').textContent = error.message; }
};
action(refresh);
