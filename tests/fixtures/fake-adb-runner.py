#!/usr/bin/python3
"""SOFTWARE FAKE launcher. Immutable executable; per-test programs are data."""
import pathlib
import sys

entry = pathlib.Path(sys.argv[0])
with entry.with_suffix('.script').open('rb') as source:
    program = source.read(65537)
if len(program) > 65536:
    raise SystemExit(2)
# Preserve the fixture's argv and __file__ contract for mode/PNG/PID sidecars.
exec(compile(program, str(entry), 'exec'), {'__name__': '__main__', '__file__': str(entry)})
