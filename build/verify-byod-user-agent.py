#!/usr/bin/env python3
"""Check a real browser's HTTP and JavaScript UA against its build version.

Uses an isolated temporary profile and a loopback-only HTTP server. No sign-in
or external test site is needed. Only the process started here is closed.
"""

import argparse
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import re
import subprocess
import tempfile
import threading
import time


PAGE = b"""<!doctype html><title>BYOD User-Agent smoke test</title>
<script>
fetch('/ua-check', {
  method: 'POST', headers: {'Content-Type': 'application/json'},
  body: JSON.stringify({userAgent: navigator.userAgent})
}).then(() => { document.body.textContent = 'UA check received'; });
</script><body>Checking browser User-Agent...</body>"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--chrome', required=True, type=Path)
    parser.add_argument('--version', required=True)
    parser.add_argument('--headless', action='store_true')
    args = parser.parse_args()
    chrome = args.chrome.resolve(strict=True)
    if not re.fullmatch(r'\d+\.\d+\.\d+\.\d+', args.version):
        parser.error('--version must be the full four-part Chromium version')
    work = Path(tempfile.mkdtemp(prefix='byod-ua-smoke-'))
    received = threading.Event()
    observed = {}

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            if self.path != '/':
                self.send_error(404)
                return
            observed['navigation'] = self.headers.get('User-Agent', '')
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Content-Length', str(len(PAGE)))
            self.end_headers()
            self.wfile.write(PAGE)

        def do_POST(self):
            size = int(self.headers.get('Content-Length', '0'))
            if self.path != '/ua-check' or not 0 < size <= 8192:
                self.send_error(400)
                return
            try:
                data = json.loads(self.rfile.read(size))
                observed['javascript'] = data['userAgent']
                observed['fetch'] = self.headers.get('User-Agent', '')
            except (ValueError, KeyError, TypeError):
                self.send_error(400)
                return
            self.send_response(204)
            self.end_headers()
            received.set()

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    command = [str(chrome), '--no-first-run', '--no-default-browser-check',
               '--disable-background-networking', '--disable-component-update',
               '--disable-sync', f'--user-data-dir={work / "profile"}']
    if args.headless:
        command.append('--headless=new')
    command.append(f'http://127.0.0.1:{server.server_port}/')
    print(f'SMOKE_LOG_DIR={work}', flush=True)
    process = None
    try:
        with (work / 'browser.log').open('wb') as log:
            process = subprocess.Popen(command, stdout=log, stderr=log)
            deadline = time.monotonic() + 60
            while not received.wait(0.5):
                if process.poll() is not None:
                    raise RuntimeError(f'browser exited: {process.returncode}')
                if time.monotonic() >= deadline:
                    raise RuntimeError('browser did not report UA within 60 seconds')
            suffix = 'BYODBrowser/' + args.version
            for surface in ('navigation', 'javascript', 'fetch'):
                ua = observed.get(surface, '')
                if ua.split().count(suffix) != 1:
                    raise RuntimeError(f'{surface}: missing/duplicate/wrong BYOD version: {ua}')
                if not ua.startswith('Mozilla/5.0 ') or ' Safari/' not in ua:
                    raise RuntimeError(f'{surface}: lost normal Chromium UA: {ua}')
            if len(set(observed.values())) != 1:
                raise RuntimeError(f'HTTP/JavaScript UA mismatch: {observed}')
            report = json.dumps(observed, indent=2)
            (work / 'user-agent.json').write_text(report + '\n', encoding='utf-8')
            print(report, flush=True)
            print('PASS: navigation, fetch, and navigator.userAgent match the build version.', flush=True)
    finally:
        if process and process.poll() is None:
            # The isolated profile ensures this PID belongs to our test alone.
            subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'],
                           check=False, capture_output=True)
            process.wait(timeout=15)
        server.shutdown()
        server.server_close()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
