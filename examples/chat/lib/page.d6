# The whole client: one page, no build step. It renders every server value
# with textContent, so chat text can't inject markup.
def chat_page()
  "<!doctype html>
<html lang='en'>
<head>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width, initial-scale=1'>
<title>Diamond chat</title>
<style>
:root{--bg:#090c14;--card:#121729;--border:#232a42;--text:#eef1fa;--dim:#9aa3bd;--cyan:#22d3ee;--blue:#3b82f6;--warn:#f0b86e;color-scheme:dark}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:16px/1.5 system-ui,sans-serif;height:100vh;display:flex;flex-direction:column}
header{padding:14px 20px;border-bottom:1px solid var(--border);display:flex;justify-content:space-between;align-items:center;gap:12px}
header h1{font-size:18px;margin:0}
#members{color:var(--dim);font-size:14px}
main{flex:1;overflow-y:auto;padding:16px 20px}
.line{margin:0 0 6px;overflow-wrap:anywhere}
.name{font-weight:700;color:var(--cyan)}
.bot{color:var(--warn)}
.note{color:var(--dim);font-style:italic}
form{display:flex;gap:8px;padding:12px 20px;border-top:1px solid var(--border)}
input{flex:1;font:inherit;color:var(--text);background:var(--card);border:1px solid var(--border);border-radius:999px;padding:10px 16px}
button{font:inherit;font-weight:600;color:var(--text);background:linear-gradient(135deg,#4f46e5,#3b82f6 55%,#22d3ee);border:0;border-radius:999px;padding:10px 18px;cursor:pointer}
</style>
</head>
<body>
<header><h1>Diamond chat</h1><span id='members'>connecting...</span></header>
<main id='log' aria-live='polite'></main>
<form id='form'><input id='text' autocomplete='off' placeholder='Say something, or /help' maxlength='500'><button>Send</button></form>
<script>
const log = document.getElementById('log');
const members = document.getElementById('members');
let name = '';
function line(parts, cls) {
  const p = document.createElement('p');
  p.className = 'line ' + (cls || '');
  for (const [text, partClass] of parts) {
    const span = document.createElement('span');
    span.textContent = text;
    if (partClass) span.className = partClass;
    p.append(span);
  }
  log.append(p);
  log.scrollTop = log.scrollHeight;
}
function show(event) {
  if (event.type === 'message') line([[event.name + ': ', 'name'], [event.text]]);
  else if (event.type === 'bot') line([['bot: ', 'name bot'], [event.text]], 'bot');
  else if (event.type === 'joined') line([[event.name + ' joined']], 'note');
  else if (event.type === 'left') line([[event.name + ' left']], 'note');
  else if (event.type === 'error') line([[event.text]], 'bot');
  if (event.members) members.textContent = event.members.length + ' online: ' + event.members.join(', ');
}
function connect() {
  name = (prompt('Your name (1-24 characters)') || '').trim() || 'guest-' + Math.floor(Math.random() * 1000);
  const scheme = location.protocol === 'https:' ? 'wss://' : 'ws://';
  const ws = new WebSocket(scheme + location.host + '/ws');
  ws.onopen = () => ws.send(JSON.stringify({type: 'join', name}));
  ws.onmessage = (message) => {
    const event = JSON.parse(message.data);
    if (event.type === 'welcome') {
      line([['You joined as ' + event.name + '. Try /help.']], 'note');
      event.history.forEach(show);
    } else show(event);
  };
  ws.onclose = () => { members.textContent = 'disconnected'; line([['Connection closed. Reload to rejoin.']], 'note'); };
  document.getElementById('form').addEventListener('submit', (submit) => {
    submit.preventDefault();
    const input = document.getElementById('text');
    if (input.value.trim() && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify({type: 'say', text: input.value}));
    input.value = '';
  });
}
connect();
</script>
</body>
</html>
"
end
