#!/usr/bin/env python3
"""Observable Kore source callback lifetime, normal/packed and optional Memcheck."""
import argparse
import http.client
import os
from pathlib import Path
import selectors
import shutil
import signal
import socket
import subprocess
import sys
import time


def scenario(command, root, memory=False):
    root.mkdir(parents=True)
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    env = dict(os.environ, VECTIS_SOURCE_PORT=str(port), TMPDIR=str(root),
               XDG_STATE_HOME=str(root/'state'), XDG_CONFIG_HOME=str(root/'config'))
    if memory:
        command = ['valgrind', '--track-origins=yes', '--leak-check=full', '--show-leak-kinds=definite,indirect,possible',
                   '--errors-for-leak-kinds=definite,indirect,possible', '--error-exitcode=99',
                   '--log-file='+str(root/'memory.%p.log')] + command
    with (root/'stderr.log').open('wb') as errors:
        process = subprocess.Popen(command, cwd=root, env=env, stdout=subprocess.PIPE, stderr=errors,
                                   start_new_session=True)
        selector = selectors.DefaultSelector()
        selector.register(process.stdout, selectors.EVENT_READ)
        pending = b''
        lines = []

        def until(predicate):
            nonlocal pending
            deadline = time.monotonic()+ (45 if memory else 10)
            while not predicate():
                remaining = deadline-time.monotonic()
                assert remaining > 0 and selector.select(remaining), ('missing event', lines)
                data = os.read(process.stdout.fileno(), 4096)
                assert data, ('premature exit', process.poll(), (root/'stderr.log').read_text(), lines)
                pending += data
                while b'\n' in pending:
                    line, pending = pending.split(b'\n', 1)
                    lines.append(line.decode())

        def request(path, method="GET", body=None):
            connection = http.client.HTTPConnection('127.0.0.1', port, timeout=45 if memory else 5)
            try:
                connection.request(method, path, body, headers={'Connection':'close'})
                response = connection.getresponse()
                return response.status, response.read()
            finally:
                connection.close()
        try:
            until(lambda: 'ready' in lines)
            for kind in ('spooled_source','stream_source'):
                assert request('/'+kind) == (200, b'x'*9000), lines
                until(lambda: kind+' close rejected' in lines)
                assert kind+' read rejected' in lines
                assert kind+' reset rejected' in lines
            for phase in ('read','reset'):
                status, _ = request('/fail-'+phase)
                assert status==500, (phase, status, lines)
                until(lambda: 'failed '+phase+' close rejected' in lines)
                assert request('/healthy') == (200, b'healthy')
            for path, body, label in (('/upload', 'abc', 'upload_source'), ('/dsv', 'id\none\n', 'dsv_source')):
                assert request(path, 'POST', body) == (200, b'owned'), lines
                until(lambda: label+' close rejected' in lines)
                assert label+' read rejected' in lines and label+' reset rejected' in lines
            assert all(phase+' rejected' in lines for phase in
                       ('upload open', 'upload chunk', 'upload complete', 'upload cleanup', 'dsv row', 'dsv complete'))
            for context in ('route','before','after','dsv','upload'):
                for phase in ('read','reset','close','content_type','headers','body'):
                    for mode in ('throw','guard'):
                        path='/convert-'+context+'-'+phase+'-'+mode
                        method='POST' if context in ('dsv','upload') else 'GET'
                        body='id\none\n' if context=='dsv' else 'abc' if context=='upload' else None
                        status, result=request(path,method,body)
                        assert status==(500 if mode=='throw' else 200), (path,status,result,lines)
                        if mode=='guard':
                            assert result==b'resolved', (path,result)
                        assert request('/healthy')==(200,b'healthy'), path
            for context in ('route', 'dsv', 'upload'):
                for phase in ('read', 'reset'):
                    for shape in ('number', 'table'):
                        path='/source-error-'+context+'-'+phase+'-'+shape
                        method='GET' if context=='route' else 'POST'
                        body=None if context=='route' else 'id\none\n' if context=='dsv' else 'abc'
                        status, _=request(path, method, body)
                        assert status==500, (path, status)
                        until(lambda: path+' close rejected' in lines)
                        assert request('/healthy')==(200, b'healthy')
            assert request('/source-error-cleanup')==(200, b'clean')
            for mode in ('throw','guard'):
                status,result=request('/convert-auth-'+mode)
                assert status==(500 if mode=='throw' else 200), (mode,status,result)
                assert request('/healthy')==(200,b'healthy')
            assert request('/conversion-ownership')==(200,b'12')
            assert request('/graceful') == (200, b'stopping')
            until(lambda: 'graceful route stop accepted' in lines)
            process.send_signal(signal.SIGTERM)
            until(lambda: 'closed' in lines)
            assert process.wait(timeout=45 if memory else 5)==0, (root/'stderr.log').read_text()
            if memory:
                logs = list(root.glob('memory.*.log'))
                assert len(logs)>=2, logs
                for log in logs:
                    text = log.read_text()
                    assert 'ERROR SUMMARY: 0 errors' in text, (log, [line for line in text.splitlines() if 'ERROR SUMMARY:' in line])
        finally:
            selector.close()
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=5)


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--vectis', required=True)
    parser.add_argument('--script', required=True)
    parser.add_argument('--work', required=True)
    parser.add_argument('--memory', action='store_true')
    args=parser.parse_args()
    args.vectis=str(Path(args.vectis).resolve())
    args.script=str(Path(args.script).resolve())
    work=Path(args.work).resolve()
    assert work.is_relative_to(Path(__file__).resolve().parents[2]/'build')
    shutil.rmtree(work, ignore_errors=True)
    scenario([args.vectis,args.script], work/'normal', args.memory)
    if sys.platform!='darwin' and not args.memory:
        packed=work/'packed'
        result=subprocess.run([args.vectis,'-a','pack','--script',args.script,'--output',str(packed)],
                              capture_output=True, timeout=15)
        assert result.returncode==0, result.stderr
        scenario([str(packed)], work/'packed-run')
    print('app source callback lifetime ok')


if __name__=='__main__':
    main()
