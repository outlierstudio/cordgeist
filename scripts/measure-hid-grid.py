#!/usr/bin/env python3
"""Explicit physical test on an owner-prepared fullscreen white screen.

Requires installed Pillow for a blank-screen guard. Uses the existing daemon/CLI
HID path, exact ADB route, observed InputReader mapping and private temporary
captures. Only numeric evidence is retained. Never an agent-acceptance substitute.
Run without python -O: assertions are deliberate test/safety preconditions.
"""
import subprocess, json, pathlib, re, threading, time, math
import argparse, tempfile, os
parser = argparse.ArgumentParser(description='Explicit owned-fullscreen nine-point HID measurement; no ADB touch injection')
parser.add_argument('--binary', required=True, type=pathlib.Path)
parser.add_argument('--runtime', required=True, type=pathlib.Path)
parser.add_argument('--output', required=True, type=pathlib.Path)
parser.add_argument('--owned-fullscreen', action='store_true', required=True)
args = parser.parse_args()
root = pathlib.Path.cwd()
base = [str(args.binary.resolve()), '--runtime', str(args.runtime.resolve())]
private_context = tempfile.TemporaryDirectory(prefix='cordgeist-hid-grid-', dir=str(root / 'build'))
private = pathlib.Path(private_context.name)
os.chmod(private, 448)
lease = str(private / 'lease.json')
inventory = subprocess.run(base + ['list', '--json'], capture_output=True, text=True, timeout=5)
devices = json.loads(inventory.stdout)['outcome']['result']['value']['devices']
assert len(devices) == 1, 'Need exact single phone'
acquired = subprocess.run(base + ['acquire', '--device', devices[0]['deviceId'], '--reason', 'owned-nine-point-measurement', '--lease-file', lease, '--json'], capture_output=True, text=True, timeout=10)
assert acquired.returncode == 0, 'Exclusive ownership unavailable; no input sent'
try:
    rows = [r.split() for r in subprocess.check_output(['/usr/bin/adb', 'devices', '-l'], text=True).splitlines()[1:] if r.strip()]
    assert len(rows) == 1 and rows[0][1] == 'device'
    tid = next((x.split(':')[1] for x in rows[0] if x.startswith('transport_id:')))
    adb = ['/usr/bin/adb', '-t', tid]
    dump = subprocess.check_output(adb + ['shell', 'dumpsys', 'input'], text=True, timeout=5)
    blocks = [b for b in re.split('(?=  Device )', dump) if 'IsExternal: true' in b and 'Touch Input Mapper (mode - direct)' in b]
    assert len(blocks) == 1
    b = blocks[0]

    def scalar(k):
        return float(re.search('^\\s*' + k + ': ([\\d.-]+)', b, re.M).group(1))
    scale = [scalar('XScale'), scalar('YScale')]
    offset = [scalar('XTranslate'), scalar('YTranslate')]
    events = subprocess.check_output(adb + ['shell', 'getevent', '-lp'], text=True, timeout=5)
    max_x = int(re.search('X: min=0, max=(\\d+)', b).group(1))
    nodes = [re.search('/dev/input/event\\d+', b).group() for b in re.split('(?=add device)', events) if 'ABS_MT_POSITION_X' in b and f'max {max_x},' in b]
    assert len(nodes) == 1
    proc = subprocess.Popen(adb + ['shell', 'timeout', '120', 'getevent', '-lt', nodes[0]], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
    node_block = next((b for b in re.split('(?=add device)', events) if nodes[0] in b))
    state = {axis: int(re.search('ABS_MT_POSITION_' + axis + '\\s*: value (\\d+)', node_block).group(1)) for axis in ['X', 'Y']}
    state['contactEvents'] = 0
    lock = threading.Lock()

    def read():
        for line in proc.stdout:
            if 'BTN_TOUCH' in line and ('DOWN' in line or line.rstrip().endswith('00000001')) or ('ABS_MT_TRACKING_ID' in line and (not line.rstrip().endswith('ffffffff'))):
                with lock:
                    state['contactEvents'] += 1
            m = re.search('ABS_MT_POSITION_([XY])\\s+([0-9a-fA-F]+)', line)
            if m:
                with lock:
                    state[m[1]] = int(m[2], 16)
    threading.Thread(target=read, daemon=True).start()
    time.sleep(0.3)
    results = []
    try:
        for index, (fx, fy) in enumerate([(x, y) for y in [0, 0.5, 1] for x in [0, 0.5, 1]]):
            path = private / f'frame-{index}.png'
            r = subprocess.run(base + ['screenshot', '--lease-file', lease, '--output', str(path), '--longest-edge', '8192', '--json'], capture_output=True, text=True, timeout=12)
            assert r.returncode == 0, 'capture failed'
            from PIL import Image
            im = Image.open(path).convert('RGB')
            sample = im.crop((0, round(im.height * 0.03), im.width, im.height)).resize((100, 100))
            assert sum((1 for y in range(100) for x in range(100) if min(sample.getpixel((x, y))) > 240)) > 9900, 'Fullscreen blank verification failed; no further input'
            frame = json.load(open(str(path) + '.meta.json'))['frame']
            size = frame['display']
            assert size == frame['image']
            point = [round((size['width'] - 1) * fx), round((size['height'] - 1) * fy)]
            with lock:
                before_contacts = state['contactEvents']
            r = subprocess.run(base + ['tap', '--lease-file', lease, '--frame', frame['id'], '--x', str(point[0]), '--y', str(point[1]), '--json'], capture_output=True, text=True, timeout=5)
            assert r.returncode == 0, 'tap not confirmed'
            value = json.loads(r.stdout)['outcome']['result']['value']
            assert value['receipt']['inputStatus'] == 'sent'
            time.sleep(0.15)
            with lock:
                raw = [state.get('X'), state.get('Y')]
            assert None not in raw, 'missing independent event axes'
            with lock:
                assert state['contactEvents'] > before_contacts, 'no new kernel contact observed'
            actual = [round(raw[i] * scale[i] + offset[i], 3) for i in range(2)]
            error = round(math.dist(point, actual), 3)
            results.append({'intended': point, 'rawHidEvent': raw, 'landedFromAndroidMapper': actual, 'errorPx': error})
            print(json.dumps(results[-1]), flush=True)
    finally:
        proc.terminate()
        proc.wait(timeout=3)
    report = {'measurement': 'kernel getevent HID coordinates transformed using observed Android InputReader scale/translate; not visual overlay OCR', 'scale': scale, 'offset': offset, 'points': results, 'maxErrorPx': max((v['errorPx'] for v in results), default=None)}
    with args.output.open('x') as output:
        output.write(json.dumps(report, indent=2) + '\n')
finally:
    try:
        subprocess.run(base + ['release', '--lease-file', lease, '--json'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
    finally:
        private_context.cleanup()
