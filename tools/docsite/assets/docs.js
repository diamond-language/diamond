// d6 documentation site: the narrow-screen navigation drawer, and keeping the
// current page visible in the sidebar.
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
})();
