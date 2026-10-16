"""Exercise the built CLI using only Python's standard library and temporary files.

No game process, original profile, clipboard, network, or persistent user state is used.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--tool', required=True, type=Path)
    args = parser.parse_args()
    tool = args.tool.resolve(strict=True)
    project = Path(__file__).resolve().parents[1]
    checks = 0
    scenarios = 0

    def check(condition, message):
        nonlocal checks
        checks += 1
        if not condition:
            raise AssertionError(message)

    def invoke(*arguments, data=b''):
        return subprocess.run([str(tool), *(str(item) for item in arguments)],
                              input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=20, check=False)

    def session(root, commands):
        lines = []
        for command in commands:
            if isinstance(command, bytes):
                lines.append(command)
            elif isinstance(command, str):
                lines.append(command.encode('utf-8'))
            else:
                lines.append(json.dumps(command, ensure_ascii=False, allow_nan=False).encode('utf-8'))
        result = invoke('script-session', root, data=b'\n'.join(lines) + (b'\n' if lines else b''))
        check(result.stderr == b'', f'JSON session wrote stderr: {result.stderr!r}')
        replies = [json.loads(line) for line in result.stdout.decode('utf-8').splitlines()]
        check(bool(replies), 'Session produced no replies')
        for index, reply in enumerate(replies, 1):
            check(all(key in reply for key in ('op', 'line', 'ok', 'results', 'logs', 'errors')),
                  f'Missing reply keys at {index}: {reply}')
            check(isinstance(reply['ok'], bool) and isinstance(reply['results'], dict)
                  and isinstance(reply['logs'], list) and isinstance(reply['errors'], list),
                  f'Wrong reply field types: {reply}')
            check(reply['ok'] == (len(reply['errors']) == 0), f'Error status mismatch: {reply}')
        return result, replies

    def texts(reply):
        return [entry.get('text') for entry in reply['logs']]

    with tempfile.TemporaryDirectory(prefix='solace-cli-session-') as temporary:
        directory = Path(temporary)
        root = directory / '中文脚本 目录'
        root.mkdir()
        sample = (project / 'examples/scripts/lifecycle.lua').read_text(encoding='utf-8')
        (root / '生命周期.lua').write_text(sample, encoding='utf-8')
        sample_commands = [json.loads(line) for line in
                           (project / 'examples/session-commands.jsonl').read_text(encoding='utf-8').splitlines()]
        sample_commands[0]['file'] = '生命周期.lua'
        result, replies = session(root, sample_commands)
        check(result.returncode == 0 and len(replies) == 12 and all(r['ok'] for r in replies), 'Example session failed')
        check(replies[0]['results']['id'] == 1, 'Initial ID differs')
        check(replies[0]['results']['scripts'][0]['file'] == '生命周期.lua', 'Unicode filename lost')
        check(texts(replies[3]) == ['delayed callback', 'present'], 'Delay must precede present')
        check(replies[8]['results']['id'] == 2 and replies[8]['results']['previous_id'] == 1, 'Reload did not replace ID')
        check(texts(replies[8]) == ['shutdown lifecycle.lua', 'loaded lifecycle.lua'], 'Reload logs differ')
        check(texts(replies[9]) == ['delayed callback', 'present'], 'Reload Delay clock differs')
        check(replies[-1]['results']['scripts'] == [], 'Close left loaded scripts')
        check('reason' not in replies[-1], 'Explicit close produced EOF reply')
        scenarios += 1

        result, replies = session(root, [{'op': 'load', 'file': '生命周期.lua'}])
        check(result.returncode == 0 and len(replies) == 2, 'EOF closure failed')
        check(replies[-1]['op'] == 'close' and replies[-1]['reason'] == 'eof' and replies[-1]['line'] == 2,
              'EOF reply missing')
        check(texts(replies[-1]) == ['shutdown lifecycle.lua'], 'EOF lost shutdown log')
        scenarios += 1

        (root / 'fail-load.lua').write_text('print("before load failure"); error("load-failure",0)', encoding='utf-8')
        result, replies = session(root, [{'op': 'load', 'file': 'fail-load.lua'}, {'op': 'list'}, {'op': 'close'}])
        check(result.returncode == 1 and not replies[0]['ok'] and replies[1]['ok'], 'Load failure continuation')
        check(texts(replies[0]) == ['before load failure'] and replies[0]['results']['scripts'] == [], 'Failed load log/state')
        scenarios += 1

        (root / 'fail-event.lua').write_text('events.presentQueue:Add(function() print("before event failure"); error("event-failure",0) end)', encoding='utf-8')
        result, replies = session(root, [{'op': 'load', 'file': 'fail-event.lua'},
                                        {'op': 'load', 'file': '生命周期.lua'},
                                        {'op': 'dispatch', 'queue': 'present', 'args': []}, {'op': 'list'}, {'op': 'close'}])
        check(result.returncode == 1 and not replies[2]['ok'], 'Event failure exit/status')
        check(texts(replies[2]) == ['before event failure', 'present'], 'Per-script failure isolation/logs')
        check(replies[2]['errors'][0]['id'] == 1 and len(replies[3]['results']['scripts']) == 1,
              'Failed event owner remains loaded')
        scenarios += 1

        (root / 'fail-shutdown.lua').write_text('function __shutdown() print("before shutdown failure"); error("shutdown-failure",0) end', encoding='utf-8')
        result, replies = session(root, [{'op': 'load', 'file': 'fail-shutdown.lua'},
                                        {'op': 'load', 'file': '生命周期.lua'}, {'op': 'close'}])
        check(result.returncode == 1 and not replies[-1]['ok'], 'Shutdown failure was swallowed')
        check(texts(replies[-1]) == ['before shutdown failure', 'shutdown lifecycle.lua'], 'Close did not continue after failure')
        check(replies[-1]['results']['scripts'] == [], 'Failed shutdown left live scripts')
        result, replies = session(root, [{'op': 'load', 'file': 'fail-shutdown.lua'}])
        check(result.returncode == 1 and replies[-1]['reason'] == 'eof' and not replies[-1]['ok'], 'EOF shutdown failure was swallowed')
        scenarios += 1

        result, replies = session(root, [{'op': 'load', 'file': 'fail-shutdown.lua'},
                                        {'op': 'reload', 'id': 1}, {'op': 'list'}, {'op': 'close'}])
        check(result.returncode == 1 and not replies[1]['ok'] and replies[2]['results']['scripts'] == [], 'Failed reload state')
        check(texts(replies[1]) == ['before shutdown failure'], 'Failed reload lost log')
        scenarios += 1

        result, replies = session(root, [{'op': 'load', 'file': '生命周期.lua'},
                                        {'op': 'load', 'file': '生命周期.lua'}, {'op': 'unload', 'id': 1}, {'op': 'close'}])
        check(result.returncode == 0 and replies[1]['results']['id'] == 1 and replies[1]['logs'] == [], 'Duplicate load created another state')
        check(replies[2]['results']['scripts'] == [] and texts(replies[2]) == ['shutdown lifecycle.lua'], 'Unload did not close explicitly')
        scenarios += 1

        invalid = [
            '', '{', '[]', '"list"', '{}', '{"op":"list","op":"close"}',
            {'op': 'list', 'extra': 1}, {'op': 'unknown'}, {'op': 1},
            {'op': 'load', 'file': 7}, {'op': 'load', 'file': ''}, {'op': 'load', 'file': 'a\0b'},
            {'op': 'unload', 'id': True}, {'op': 'unload', 'id': 1.0},
            {'op': 'unload', 'id': 0}, {'op': 'unload', 'id': -1},
            {'op': 'unload', 'id': 2**32}, {'op': 'unload', 'id': 2**64},
            {'op': 'dispatch', 'queue': 'event', 'args': []},
            {'op': 'dispatch', 'queue': 'create_move', 'args': []},
            {'op': 'dispatch', 'queue': 'present', 'args': None},
            {'op': 'dispatch', 'queue': 'present', 'args': [1]},
            {'op': 'dispatch', 'queue': 'frame_stage', 'args': [1.0]},
            {'op': 'dispatch', 'queue': 'frame_stage', 'args': [True]},
            {'op': 'dispatch', 'queue': 'frame_stage', 'args': [2**63]},
            {'op': 'dispatch', 'queue': 'frame_stage', 'args': [-2**63-1]},
            {'op': 'dispatch', 'queue': 'input', 'args': [1, 2, '3']},
            {'op': 'dispatch', 'queue': 'menu_toggled', 'args': [1]},
            {'op': 'tick', 'time': True}, {'op': 'tick', 'time': '0.5'},
            {'op': 'tick', 'time': 1e100}, {'op': 'tick', 'time': 1e-100},
            '{"op":"tick","time":1e999}', {'op': 'tick'}, b'{"op":"\xff"}',
        ]
        result, replies = session(root, invalid + [{'op': 'list'}, {'op': 'close'}])
        check(result.returncode == 1 and len(replies) == len(invalid)+2, 'Invalid commands interrupted JSON stream')
        check(all(not r['ok'] for r in replies[:len(invalid)]) and all(r['ok'] for r in replies[-2:]), 'Validation accepted bad input')
        check([r['line'] for r in replies] == list(range(1, len(replies)+1)), 'Line association differs')
        scenarios += 1

        (root / 'integer.lua').write_text('events.frameStageNotify:Add(function(v) assert(math.type(v)=="integer"); print(v) end)', encoding='utf-8')
        result, replies = session(root, [{'op': 'load', 'file': 'integer.lua'},
                                        {'op': 'dispatch', 'queue': 'frame_stage', 'args': [-2**63]},
                                        {'op': 'dispatch', 'queue': 'frame_stage', 'args': [2**63-1]},
                                        {'op': 'tick', 'time': -1}, {'op': 'close'}])
        check(result.returncode == 0 and all(r['ok'] for r in replies), 'Valid integer boundaries rejected')
        check(texts(replies[1]) == [str(-2**63)] and texts(replies[2]) == [str(2**63-1)], 'Boundary integer narrowed/wrapped')
        scenarios += 1

        outside = directory / 'outside.lua'
        outside.write_text('error("outside must not execute")', encoding='utf-8')
        result, replies = session(root, [{'op': 'load', 'file': '../outside.lua'},
                                        {'op': 'load', 'file': str(root / '生命周期.lua')},
                                        {'op': 'load', 'file': 'missing.lua'}, {'op': 'list'}, {'op': 'close'}])
        check(result.returncode == 1 and all(not r['ok'] for r in replies[:3]), 'Invalid path accepted')
        check(replies[3]['results']['scripts'] == [], 'Invalid path changed session')
        scenarios += 1

        (root / 'binary-log.lua').write_text('print(string.char(255,254)); function __shutdown() print("closed") end', encoding='utf-8')
        result, replies = session(root, [{'op': 'load', 'file': 'binary-log.lua'}, {'op': 'close'}])
        check(result.returncode == 0 and replies[0]['ok'], 'Binary log broke JSON output')
        check(replies[0]['logs'][0]['text'] is None and replies[0]['logs'][0]['text_bytes'] == [255,254], 'Binary log bytes were lost')
        scenarios += 1

        result, replies = session(directory / 'missing-root', [])
        check(result.returncode == 1 and len(replies) == 1 and replies[0]['op'] == 'startup' and replies[0]['line'] == 0,
              'Startup failure protocol differs')
        scenarios += 1

        (root / '运行失败.lua').write_text('function __shutdown() print("shutdown printed"); error("shutdown-failure",0) end; print("run printed"); error("run-failure",0)', encoding='utf-8')
        result = invoke('run-lua', root / '运行失败.lua')
        check(result.returncode == 1, 'run-lua failure exit status')
        check(result.stdout.decode('utf-8').splitlines() == ['run printed', 'shutdown printed'], 'run-lua lost prior/shutdown logs')
        check(b'run-failure' in result.stderr and b'shutdown-failure' in result.stderr, 'run-lua lost one error')
        result = invoke('run-lua', root / '生命周期.lua')
        check(result.returncode == 0 and result.stderr == b'', 'run-lua Unicode path failed')
        check(result.stdout.decode('utf-8').splitlines() == ['loaded lifecycle.lua', 'shutdown lifecycle.lua'],
              'run-lua synthesized events or lost shutdown')
        scenarios += 1

        config = root / '临时配置.json'
        config.write_text('{"version":3,"fields":{}}', encoding='utf-8')
        result = invoke('inspect-config', config)
        check(result.returncode == 0 and json.loads(result.stdout)['field_count'] == 0, 'Wide inspect-config path failed')
        copy = root / '写入配置.json'
        result = invoke('roundtrip-config', config, copy)
        check(result.returncode == 0 and json.loads(copy.read_text(encoding='utf-8')) == json.loads(config.read_text(encoding='utf-8')),
              'Wide roundtrip-config path failed')
        # An existing fixed-byte UTF-8 key vector also validates wchar-to-UTF-8
        # conversion across the command-line boundary, including byte truncation.
        result = invoke('key', 'a'*119+'é', 'x')
        check(result.returncode == 0 and result.stdout.strip() == b'218ad8d9', 'Key argument is not strict UTF-8')
        scenarios += 1

    print(json.dumps({'status': 'passed', 'scenarios': scenarios, 'checks': checks}, sort_keys=True))


if __name__ == '__main__':
    main()
