# usage: labini.py <ini> key=value ...   (sets keys in the [Lab] section, keeps everything else)
import sys
path, pairs = sys.argv[1], dict(a.split('=', 1) for a in sys.argv[2:])
lines = open(path).read().splitlines()
out, sec, done = [], None, set()
for ln in lines:
    s = ln.strip()
    if s.startswith('['):
        if sec == '[Lab]':
            out.extend('%s=%s' % (k, v) for k, v in pairs.items() if k not in done); done.update(pairs)
        sec = s
    elif sec == '[Lab]' and '=' in s and s.split('=', 1)[0] in pairs:
        k = s.split('=', 1)[0]; ln = '%s=%s' % (k, pairs[k]); done.add(k)
    out.append(ln)
if sec == '[Lab]': out.extend('%s=%s' % (k, v) for k, v in pairs.items() if k not in done)
open(path, 'w', newline='\r\n').write('\n'.join(out) + '\n')
