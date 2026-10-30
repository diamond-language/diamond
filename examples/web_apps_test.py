"""Real HTTP contracts for the three local web apps, using isolated SQLite files."""
import contextlib
import http.client
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time

DIAMOND, ROOT = sys.argv[1], Path(__file__).resolve().parent.parent
BINARIES = Path(sys.argv[2]) if len(sys.argv) > 2 else None


def reserve_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def request(port, path, data=None, *, headers=None, raw=False):
    conn = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
    try:
        body = None if data is None else json.dumps(data)
        head = {'Content-Type': 'application/json'}
        head.update(headers or {})
        conn.request('GET' if data is None else 'POST', path, body, head)
        response = conn.getresponse()
        content = response.read().decode()
        return response.status, content if raw else json.loads(content)
    finally:
        conn.close()


@contextlib.contextmanager
def server(app, database, cwd):
    port = reserve_port()
    entry = 'service.d6' if app == 'job_service' else 'web.d6'
    command = [DIAMOND, str(ROOT / 'examples' / app / entry), str(port)]
    if database is not None:
        command += [str(database), str(ROOT / 'examples' / app / 'web')]
    if BINARIES:
        command = [str(BINARIES / app), *command[2:]]
    with tempfile.TemporaryFile(mode='w+') as log:
        child = subprocess.Popen(command, cwd=cwd, stdout=log, stderr=log, env={**os.environ, 'DIAMOND_NO_CACHE': '1'})
        try:
            deadline = time.monotonic() + 15
            while True:
                if child.poll() is not None:
                    log.seek(0)
                    raise AssertionError(log.read())
                try:
                    if request(port, '/health')[0] == 200:
                        break
                except (OSError, http.client.HTTPException):
                    pass
                assert time.monotonic() < deadline, f'{app} startup timed out'
                time.sleep(.05)
            for path in ['/', '/app.js', '/common.js', '/base.css', '/style.css']:
                status, text = request(port, path, raw=True)
                assert status == 200 and text, (app, path, status, text)
            assert request(port, '/../../AGENTS.md')[0] == 404
            assert request(port, '/health', headers={'Host': 'attacker.example'})[0] == 403
            assert request(port, '/missing', {}, headers={'Origin': 'https://attacker.example'})[0] == 403
            yield port
        finally:
            if child.poll() is None:
                child.terminate()
            try:
                result = child.wait(timeout=8)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
                raise AssertionError(f'{app} failed to stop')
            log.seek(0)
            assert result == 0, log.read()


def await_job(port, job, status):
    deadline = time.monotonic() + 10
    while True:
        code, value = request(port, f'/jobs/{job}')
        assert code == 200, value
        if value['status'] == status:
            return value
        assert time.monotonic() < deadline, value
        time.sleep(.02)


def run(cwd):
    with tempfile.TemporaryDirectory() as work:
        # Exercise documented optional database/assets arguments, too.
        launch = Path(work) / 'launch'
        launch.mkdir()
        (launch / 'cuts').symlink_to(cwd / 'cuts', target_is_directory=True)
        (launch / 'examples').symlink_to(ROOT / 'examples', target_is_directory=True)
        for app in ['task_board', 'bookmarks']:
            with server(app, None, launch) as port:
                assert request(port, '/health')[0] == 200
        board = Path(work) / 'board.db'
        with server('task_board', board, cwd) as port:
            assert request(port, '/api/board')[1] == {'projects': [], 'tasks': []}
            assert request(port, '/api/projects', {'name': ''})[0] == 422
            assert request(port, '/api/projects', {'name': 'Diamond'})[0] == 200
            assert request(port, '/api/projects', {'name': 'Diamond'})[0] == 422
            assert request(port, '/api/tasks', {'project_id': 999, 'title': 'Orphan'})[0] == 422
            assert request(port, '/api/tasks', {'project_id': 1, 'title': '<script>plain text</script>'})[0] == 200
            assert request(port, '/api/tasks/1', {'status': 'doing', 'notes': 'Shared with the TUI'})[0] == 200
            assert request(port, '/api/tasks/1', {'status': 'invalid'})[0] == 422
            assert request(port, '/api/tasks/999', {'title': 'missing'})[0] == 404
        # A separate CLI process observes the web edit through the shared store.
        cli = subprocess.run([DIAMOND, str(ROOT / 'examples/task_board/app.d6'), '--db', str(board), 'show', '1'], cwd=cwd, capture_output=True, text=True, check=True)
        assert '[doing]' in cli.stdout and 'Shared with the TUI' in cli.stdout
        with server('task_board', board, cwd) as port:
            assert request(port, '/api/board')[1]['tasks'][0]['notes'] == 'Shared with the TUI'

        bookmarks = Path(work) / 'bookmarks.db'
        # Opening the app migrates the original example's schema in place.
        with sqlite3.connect(bookmarks) as db:
            db.execute('CREATE TABLE bookmarks (id INTEGER PRIMARY KEY, url TEXT NOT NULL UNIQUE, title TEXT NOT NULL)')
            db.execute("INSERT INTO bookmarks(url,title) VALUES ('https://example.com/legacy','Legacy')")
        with server('bookmarks', bookmarks, cwd) as port:
            assert request(port, '/api/bookmarks')[1][0]['tags'] == ''
            assert request(port, '/api/bookmarks', {'url': 'http://127.0.0.1/admin', 'title': 'Private'})[0] == 422
            assert request(port, '/api/bookmarks', {'url': 'https://example.com/new', 'title': 'New', 'tags': 'Diamond, reading, Diamond'})[0] == 201
            exported = request(port, '/api/export')[1]
            assert exported[-1]['tags'] == 'diamond, reading'
            assert request(port, '/api/import', {'bookmarks': exported})[1] == {'added': 0, 'refused': 2}
            assert request(port, '/api/import', {'text': 'https://example.com/import Imported\nhttp://localhost/ No'})[1] == {'added': 1, 'refused': 1}
        with server('bookmarks', bookmarks, cwd) as port:
            assert len(request(port, '/api/bookmarks')[1]) == 3

        jobs = Path(work) / 'jobs.db'
        with server('job_service', jobs, cwd) as port:
            def enqueue(csv, **extra):
                code, value = request(port, '/jobs', {'csv': csv, **extra})
                assert code == 202, value
                return value['id']
            job = enqueue('name,email\r\n"Ada, A.",ada@example.com\r\n"Grace ""Amazing"" Hopper",grace@example.com\r\n')
            value = await_job(port, job, 'succeeded')
            assert value['progress'] == value['total'] == value['result'] == 2, value
            assert json.loads(value['summary']) == {'imported': 2}
            assert len(request(port, '/contacts')[1]) == 2
            bad = enqueue('name,email\nValid,valid@example.com\nInvalid,nope\n')
            value = await_job(port, bad, 'failed')
            assert json.loads(value['summary'])['errors'][0]['row'] == 3, value
            assert len(request(port, '/contacts')[1]) == 2, 'partial import leaked'
            malformed = enqueue('name,email\n"never closed')
            assert 'quote' in await_job(port, malformed, 'failed')['last_error']
            expired = enqueue('name,email\nNew,new@example.com\n', timeout_ms=0)
            await_job(port, expired, 'timed_out')
            # Cancel a queued CSV job behind a deterministic long-running count job.
            blocker = request(port, '/jobs', {'work_ms': 60000, 'timeout_ms': 60000})[1]['id']
            await_job(port, blocker, 'running')
            cancelled = enqueue('name,email\nCancelled,cancel@example.com\n')
            assert request(port, f'/jobs/{cancelled}/cancel', {})[1]['status'] == 'cancelled'
            request(port, f'/jobs/{blocker}/cancel', {})
            await_job(port, blocker, 'cancelled')
            assert len(request(port, '/contacts')[1]) == 2
            request(port, f'/jobs/{cancelled}/retry', {})
            await_job(port, cancelled, 'succeeded')
            # Replay updates an existing email instead of inserting a duplicate.
            replay = enqueue('name,email\nAda Updated,ada@example.com\n')
            await_job(port, replay, 'succeeded')
            assert len(request(port, '/contacts')[1]) == 3
            assert len(request(port, '/jobs')[1]) >= 6
        with server('job_service', jobs, cwd) as port:
            assert len(request(port, '/contacts')[1]) == 3
    print('task board, bookmarks, and CSV jobs web checks passed')


run(Path.cwd())
