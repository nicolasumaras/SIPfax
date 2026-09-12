"""Check V34 Table 11 / equation 9-32 against actual encoder trace inputs.

No hardware or captured user data; only generated all-ones B1 material.
"""
import os,json,subprocess,tempfile,hashlib,sys,argparse
from pathlib import Path
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('--output', type=Path)
args=parser.parse_args()
binary=args.binary.resolve()
results=[]
for taps in ('0,0,0,0,0,0','4413,1769,-3741,423,2586,-446'):
 with tempfile.TemporaryDirectory() as tmp:
  p=Path(tmp);env={k:v for k,v in os.environ.items() if not k.startswith('SIPFAX_')}
  env.update(SIPFAX_B1_REFERENCE=str(p/'symbols'),SIPFAX_B1_RATE='12000',SIPFAX_B1_TRELLIS='64',SIPFAX_SHAPE='0',SIPFAX_B1_H=taps,SIPFAX_ENCDUMP=str(p/'encoder'),SIPFAX_PC_TXDBG=str(p/'precoder'))
  subprocess.run([str(binary)],env=env,capture_output=True,check=True)
  enc=[list(map(int,x.split())) for x in (p/'encoder').read_text().splitlines()]
  pc=[list(map(int,x.split())) for x in (p/'precoder').read_text().splitlines()]
  assert len(enc)==60 and len(pc)==120
  wrong=[]
  for m,(z0,z1,used,v0,position,half,state) in enumerate(enc):
   # c(n) is computed before u(n) enters the precoder. Table11 step4
   # combines current c(2m), c(2m+1), before mapping the second symbol.
   c0=(sum(pc[2*m][4:6])+sum(pc[2*m+1][4:6]))//2 & 1
   expected=(state&1)^c0^v0
   if used!=expected:wrong.append({'fourDSymbol':m,'usedU0':used,'requiredU0':expected,'Y0':state&1,'currentC0':c0,'currentV0':v0})
  results.append({'taps':taps,'checked4DSymbols':60,'equation932Violations':len(wrong),'firstViolations':wrong[:5]})
a={'binarySha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'runs':results,'basis':'ITU-T V.34 (02/98) Table11 steps4-6 and equation9-32: current U0(m)=Y0(m) xor C0(m) xor V0(m); not previous interval C0/V0. Encoder trace records U0 used in second-symbol rotation before advancing convolutional state.','scope':'Generated B1 diagnostic, not hardware qualification.'}
if args.output: args.output.write_text(json.dumps(a,indent=2)+'\n')
print(json.dumps(a,indent=2))
sys.exit(1 if any(r['equation932Violations'] for r in results) else 0)
