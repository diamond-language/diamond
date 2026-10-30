"""Offline HTTP fixture: redirects, failures, timeouts, ordering and cancellation."""
import http.server
import json
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import threading
import time

DIAMOND = sys.argv[1]
ENTRY = Path(__file__).with_name('check.d6')


class Handler(http.server.BaseHTTPRequestHandler):
    def do_HEAD(self):
        if self.path == '/slow':
            time.sleep(.8)
        status = 404 if self.path == '/missing' else 302 if self.path in ['/redirect', '/loop'] else 200
        self.send_response(status)
        if status == 302:
            self.send_header('Location', '/ok' if self.path == '/redirect' else '/loop')
        self.end_headers()

    def log_message(self, *args):
        pass


server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
try:
    with tempfile.TemporaryDirectory() as work:
        urls = Path(work) / 'urls.txt'
        base = f'http://127.0.0.1:{server.server_port}'
        urls.write_text('\n# fixture\n' + '\n'.join(base + p for p in ['/ok', '/redirect', '/missing', '/loop', '/slow']) + '\n')
        commands = [[DIAMOND, str(ENTRY)]]
        if len(sys.argv) > 2:
            commands.append([sys.argv[2]])
        for command in commands:
            result = subprocess.run(command + ['--json', '--workers', '2', '--timeout-ms', '100', str(urls)], capture_output=True, text=True, timeout=15)
            assert result.returncode == 1, (result.stdout, result.stderr)
            rows = json.loads(result.stdout)
            assert [r['url'] for r in rows] == [base + p for p in ['/ok', '/redirect', '/missing', '/loop', '/slow']]
            assert [r['status'] for r in rows[:3]] == [200, 200, 404], rows
            assert rows[3]['error'] and rows[4]['error'], rows
            urls.write_text(base + '/ok\n')
            result = subprocess.run(command + [str(urls)], capture_output=True, text=True, timeout=10)
            assert result.returncode == 0 and 'OK ' in result.stdout, result
            result = subprocess.run(command + ['--workers', '0', str(urls)], capture_output=True, text=True, timeout=10)
            assert result.returncode == 64, result
            result = subprocess.run(command + ['--workers'], capture_output=True, text=True, timeout=10)
            assert result.returncode == 64 and 'missing value' in result.stderr, result
            urls.write_text((base + '/slow\n') * 50)
            child = subprocess.Popen(command + ['--workers', '1', '--timeout-ms', '1000', str(urls)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            time.sleep(.4)
            child.send_signal(signal.SIGINT)
            out, err = child.communicate(timeout=8)
            assert child.returncode == 130 and 'cancelled' in err, (child.returncode, out, err)
            urls.write_text('\n# fixture\n' + '\n'.join(base + p for p in ['/ok', '/redirect', '/missing', '/loop', '/slow']) + '\n')
    print('site checker HTTP, timeout, ordering and cancellation checks passed')
finally:
    server.shutdown()
    server.server_close()
