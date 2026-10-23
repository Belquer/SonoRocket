# Reads the C++ marks/results (stdin log) and the listener recording; prints the verdict.
import json, math, re, sys
log = open(sys.argv[1]).read(); rec = json.load(open(sys.argv[2]))
mark = {m: int(v) for m, v in re.findall(r'MARK (\w+) (\d+)', log)}
res = {m: float(v) for m, v in re.findall(r'RESULT (\w+) (-?[\d.]+)', log)}
def db(xs): return 10 * math.log10(sum(x * x for x in xs) / len(xs)) if xs and sum(xs) > 0 else -200
fr = rec['frames']
echo_out = db([f['rms'] for f in fr if mark['start'] + 3000 <= f['t'] < mark['voice']])
voice_out = db([f['rms'] for f in fr if mark['voice'] + 300 <= f['t'] < mark['listen']])
erle = res['echo_in_dB'] - echo_out
print(f"talk flag on listener: {rec['talk']}, master blocks received: {rec['audioBlocks']}")
print(f"echo only   : in {res['echo_in_dB']:.1f} dB → out {echo_out:.1f} dB  (echo removed: {erle:.1f} dB)")
print(f"voice+echo  : voice in {res['voice_in_dB']:.1f} dB → out {voice_out:.1f} dB")
back_out = db([f['rms'] for f in fr if mark['listen'] + 400 <= f['t'] < mark['listen'] + 1200])
dt_out = db([f['rms'] for f in fr if mark['listen'] + 1450 <= f['t'] < mark['listen'] + 1850])
print(f"talking over the listener (double talk): voice in {res['voice_in_dB']:.1f} dB → out {dt_out:.1f} dB")
print(f"listener's own voice leaking back: in {res['voice_echo_in_dB']:.1f} dB → sent back {back_out:.1f} dB  (removed: {res['voice_echo_in_dB'] - back_out:.1f} dB)")
lat = mark.get('heard', 0) - rec['sentAt'] if rec['sentAt'] else None
print(f"listener → sender voice: heard after {lat} ms, level {res['out_phase2_dB']:.1f} dB")
ok = rec['talk'] and erle > 20 and voice_out > res['voice_in_dB'] - 10 and lat is not None and 0 < lat < 200 and res['dropped'] == 0 and dt_out > res['voice_in_dB'] - 10 and res['voice_echo_in_dB'] - back_out > float(__import__('os').environ.get('VOICE_ECHO_MIN', '15'))
print('PASS' if ok else 'FAIL'); sys.exit(0 if ok else 1)
