#!/usr/bin/env python3
"""Exercise real HTTP and persistence; every wait and child lifetime is bounded."""
import json
import pathlib
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

command = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix='diamond-job-service-') as directory:
    root = pathlib.Path(directory)
    database = root / 'jobs.sqlite'
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]
    base = f'http://127.0.0.1:{port}'
    process = None
    log = open(root / 'server.log', 'w+')

    def request(path, data=None):
        payload = None if data is None else json.dumps(data).encode()
        req = urllib.request.Request(base + path, data=payload,
                                     headers={'Content-Type': 'application/json'})
        try:
            response = urllib.request.urlopen(req, timeout=2)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            return response.status, json.load(response)

    def wait_for(probe, message, seconds=12):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            value = probe()
            if value:
                return value
            time.sleep(.03)
        raise AssertionError(message)

    def start():
        global process
        process = subprocess.Popen(command + [str(port), str(database)],
                                   stdout=log, stderr=log)
        def ready():
            assert process.poll() is None, 'server exited during startup'
            try:
                return request('/health')[0] == 200
            except (OSError, urllib.error.URLError):
                return False
        wait_for(ready, 'server never became ready')

    def stop():
        process.terminate()
        assert process.wait(timeout=5) == 0, 'unclean shutdown'

    def enqueue(**args):
        status, body = request('/jobs', args)
        assert status == 202, (status, body)
        return body['id']

    def state(job):
        status, body = request(f'/jobs/{job}')
        assert status == 200, (status, body)
        return body

    def await_state(job, expected):
        return wait_for(lambda: state(job) if state(job)['status'] == expected else None,
                        f'job {job} did not reach {expected}')

    def stored(job):
        with sqlite3.connect(database) as db:
            return db.execute('SELECT status, attempts FROM jobs WHERE id = ?', (job,)).fetchone()

    try:
        start()
        assert request('/jobs', {'work_ms': -1})[0] == 400
        assert request('/jobs', [1, 2])[0] == 400
        assert request('/jobs/999999')[0] == 404
        normal = enqueue(work_ms=20)
        assert await_state(normal, 'succeeded')['result'] == 20
        assert request(f'/jobs/{normal}/cancel', {})[1]['status'] == 'succeeded'
        timed = enqueue(work_ms=500, timeout_ms=30)
        assert await_state(timed, 'timed_out')['attempts'] == 0
        immediate = enqueue(work_ms=0, timeout_ms=0)
        await_state(immediate, 'timed_out')
        retry = enqueue(work_ms=0, fail_until=1, max_attempts=2)
        assert await_state(retry, 'succeeded')['attempts'] == 1
        failed = enqueue(work_ms=0, fail_until=3, max_attempts=1)
        assert await_state(failed, 'failed')['attempts'] == 1
        running = enqueue(work_ms=60000, timeout_ms=60000)
        await_state(running, 'running')
        queued = enqueue(work_ms=0)
        assert request(f'/jobs/{queued}/cancel', {})[1]['status'] == 'cancelled'
        request(f'/jobs/{running}/cancel', {})
        await_state(running, 'cancelled')
        interrupted = enqueue(work_ms=5000, timeout_ms=15000)
        await_state(interrupted, 'running')
        # An incomplete request must drain/expire without bypassing the
        # application's cancellation and database cleanup.
        with socket.create_connection(('127.0.0.1', port), timeout=2) as stalled, \
                socket.create_connection(('127.0.0.1', port), timeout=2) as silent:
            stalled.sendall(b'GET /health HTTP/1.1\r\nHost: localhost\r\n')
            time.sleep(.05)
            stop()
            assert stalled.recv(1) == b'', 'shutdown left a connection open'
            assert silent.recv(1) == b'', 'shutdown left a silent client open'
        assert stored(interrupted) == ('pending', 0), stored(interrupted)
        start()
        await_state(interrupted, 'succeeded')
        assert state(queued)['status'] == 'cancelled', 'queued cancellation was lost'
        abandoned = enqueue(work_ms=1500, timeout_ms=5000)
        await_state(abandoned, 'running')
        process.kill()
        process.wait(timeout=5)
        assert stored(abandoned)[0] == 'running'
        start()
        await_state(abandoned, 'succeeded')
        stop()
        print('job service lifecycle tests passed')
    except BaseException:
        log.flush()
        print((root / 'server.log').read_text(), file=sys.stderr)
        raise
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        log.close()
