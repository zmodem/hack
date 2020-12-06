#!/usr/bin/env python

r"""Small tool to produce input data for https://github.com/thorduri/ministat

Example:
    bench.py -o before.txt out/gn/bin/ld64.lld.darwinnew @response.txt
    # ...rebuild lld with some change...
    bench.py -o after.txt out/gn/bin/ld64.lld.darwinnew @response.txt

    ministat before.txt after.txt

"""

from __future__ import print_function
import argparse
import subprocess
import sys
import time

parser = argparse.ArgumentParser(
             epilog=__doc__,
             formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('-n', help='number of repetitions', default=5, type=int)
parser.add_argument('--output', '-o', help='write timing output to this file')
parser.add_argument('cmd', nargs=argparse.REMAINDER, help='command to time')
args = parser.parse_args()

# Make `bench.py -o foo -- cmd` work
if args.cmd and args.cmd[0] == '--':
  del args.cmd[0]

subprocess.call(args.cmd)  # Warmup

out = open(args.output, 'w') if args.output else sys.stdout

for _ in range(args.n):
  t = time.time()
  subprocess.call(args.cmd)
  e = time.time() - t
  print(e, file=out)

