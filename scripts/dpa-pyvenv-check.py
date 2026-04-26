#!/usr/bin/env python3
import sys, os
if sys.prefix != sys.base_prefix or os.environ.get('VIRTUAL_ENV'):
    sys.exit(0)
print('WARNING: Not in a venv. Plugin will install system-wide.', file=sys.stderr)
sys.exit(0 if input('Continue? [y/N] ').lower() == 'y' else 1)