// d6 documentation site: the narrow-screen navigation drawer, keeping the
// current page visible in the sidebar, and Run buttons on guide examples.
(() => {
  const toggle = document.querySelector('.nav-toggle');
  const setOpen = (open) => {
    document.body.classList.toggle('nav-open', open);
    toggle.setAttribute('aria-expanded', String(open));
  };
  toggle.addEventListener('click', () => setOpen(!document.body.classList.contains('nav-open')));
  document.addEventListener('keydown', (event) => { if (event.key === 'Escape') setOpen(false); });
  document.querySelector('.content').addEventListener('click', () => setOpen(false));

  const current = document.querySelector('.sidebar a.current');
  if (current) current.scrollIntoView({ block: 'center' });

  // Run buttons. The interpreter is the homepage REPL's WebAssembly build, served
  // from the site root; it is fetched on the first click, not on page load.
  const article = document.querySelector('article[data-runnable]');
  if (!article) return;
  const root = document.body.dataset.siteRoot || '../../';
  let interpreter = null;
  const loadInterpreter = () => {
    interpreter ??= import(new URL(`${root}js/diamond-wasm.js`, location.href).href)
      .then(({ DiamondRepl }) => DiamondRepl.load(new URL(`${root}wasm/diamond.wasm`, location.href).href))
      .catch((error) => { interpreter = null; throw error; });
    return interpreter;
  };

  const show = (panel, text, failed) => {
    panel.hidden = false;
    panel.classList.toggle('run-error', failed);
    panel.textContent = text;
  };

  article.querySelectorAll('pre > code.language-d6:not([data-run="no"])').forEach((code) => {
    const pre = code.parentElement;
    const wrapper = document.createElement('div');
    wrapper.className = 'runnable';
    pre.replaceWith(wrapper);
    wrapper.append(pre);

    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'run-button';
    button.textContent = 'Run';
    button.setAttribute('aria-label', 'Run this example');
    wrapper.append(button);

    const panel = document.createElement('pre');
    panel.className = 'run-output';
    panel.hidden = true;
    panel.setAttribute('aria-live', 'polite');
    wrapper.append(panel);

    button.addEventListener('click', async () => {
      button.disabled = true;
      button.textContent = interpreter ? 'Running…' : 'Loading…';
      try {
        const repl = await loadInterpreter();
        button.textContent = 'Running…';
        const result = repl.run(code.textContent);
        const parts = [result.output.replace(/\n$/, ''), result.error].filter((part) => part !== '');
        show(panel, parts.length ? parts.join('\n') : '(no output)', result.status !== 0);
      } catch (error) {
        show(panel, `Could not load the interpreter: ${error.message}`, true);
      } finally {
        button.disabled = false;
        button.textContent = 'Run';
      }
    });
  });
})();
