#!/usr/bin/env python3
"""Run the actual daemon dispatcher against a bounded dependency fixture.

Optionally compare a pre-fix Git revision. No Redis/kernel/SAI or route-ring
behavior is implemented by the fixture. See README.md for the exact boundary.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
REPO = next(p for p in HERE.parents if (p / 'cfgmgr/intfmgrd.cpp').is_file())


def definition(text, signature):
    if text.count(signature) != 1:
        raise ValueError('Expected one production definition: ' + signature)
    start = text.index(signature)
    masked = re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    lambda match: ' ' * len(match.group()), text, flags=re.S)
    brace = masked.index('{', start)
    depth = 0
    for end in range(brace, len(masked)):
        depth += (masked[end] == '{') - (masked[end] == '}')
        if depth == 0:
            return start, end + 1
    raise ValueError('Unbalanced production definition: ' + signature)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--baseline', help='Optional pre-fix revision for expected behavioral RED control')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix='dispatcher-', dir=args.out.resolve()))
    records = []

    def step(name, command, expected=0, summary=None):
        result = subprocess.run(list(map(str, command)), cwd=REPO, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, timeout=120)
        (out / (name + '.log')).write_text(result.stdout)
        records.append({'name': name, 'argv': list(map(str, command)), 'exit_code': result.returncode})
        (out / 'results.json').write_text(json.dumps(records, indent=2) + '\n')
        print(name, 'exit', result.returncode, flush=True)
        print(result.stdout, end='', flush=True)
        if result.returncode != expected:
            raise RuntimeError('Unexpected command status: ' + name)
        if summary is not None:
            actual = re.findall(r'^SUMMARY passed=(\d+) failed=(\d+)$', result.stdout, re.M)
            if actual != [tuple(map(str, summary))]:
                raise RuntimeError('Unexpected test outcomes: ' + name)

    variants = [('baseline', args.baseline)] if args.baseline else []
    variants.append(('candidate', None))
    for variant, revision in variants:
        build = out / variant
        build.mkdir()
        sources = {}
        manifest = {'revision': revision, 'files': {}, 'definitions': {}}
        for name in ('cfgmgr/intfmgrd.cpp', 'cfgmgr/intfmgr.cpp', 'orchagent/orch.cpp'):
            data = subprocess.check_output(['git', 'show', revision + ':' + name], cwd=REPO) if revision else (REPO / name).read_bytes()
            sources[name] = data.decode()
            manifest['files'][name] = hashlib.sha256(data).hexdigest()
        (build / 'intfmgrd_under_test.cpp').write_text(sources['cfgmgr/intfmgrd.cpp'])
        for name, path, signature in (
            ('orch_sweep.inc', 'orchagent/orch.cpp', 'void Orch::doTask()'),
            ('consumer_execute.inc', 'orchagent/orch.cpp', 'void Consumer::execute()'),
            ('consumer_drain.inc', 'orchagent/orch.cpp', 'void Consumer::drain()'),
            ('intfmgr_handler.inc', 'cfgmgr/intfmgr.cpp', 'void IntfMgr::doTask(Consumer &consumer)'),
        ):
            start, end = definition(sources[path], signature)
            code = sources[path][start:end]
            first_line = sources[path][:start].count('\n') + 1
            (build / name).write_text('#line %d "%s"\n%s\n' % (first_line, path, code))
            manifest['definitions'][name] = {'source': path, 'first_line': first_line, 'sha256': hashlib.sha256(code.encode()).hexdigest()}
        for name in ('dbconnector.h', 'select.h', 'exec.h', 'schema.h', 'intfmgr.h', 'warm_restart.h', 'notificationconsumer.h'):
            (build / name).write_text('#include "fixture.hpp"\n')
        (build / 'source-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        command = shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++14', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
            '-I' + str(build), '-I' + str(HERE), str(HERE / 'retry_test.cpp')]
        if not revision:
            command.append('-DTEST_GUARD_ACK')
        step(variant + '-build', command + ['-o', build / 'test'])
        step(variant + '-run', [build / 'test'], 1 if revision else 0,
             (5, 4) if revision else (10, 0))
        if not revision:
            step('ubsan-build', command + ['-fsanitize=undefined', '-fno-sanitize-recover=all', '-o', build / 'test-ubsan'])
            step('ubsan-run', [build / 'test-ubsan'], summary=(10, 0))
    print('PASS source-linked dispatcher regression; evidence:', out)


if __name__ == '__main__':
    main()
