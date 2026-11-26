#!/usr/bin/env python3
"""Fail a native Vulkan fixture when validation reports an error despite exit 0."""
import re
import subprocess
import sys


def main(command):
    if not command:
        raise SystemExit('usage: validation_run.py EXECUTABLE [ARGS...]')
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, errors='replace')
    sys.stdout.write(result.stdout)
    sys.stdout.flush()
    if result.returncode:
        return result.returncode if result.returncode > 0 else 1
    if re.search(r'Validation Error|VUID-\S+', result.stdout, re.IGNORECASE):
        print('Vulkan validation rejected this fixture despite child exit 0.', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
