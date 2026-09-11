#!/usr/bin/env python3
"""Preview audited obsolete branches; --apply backs up and deletes exact refs."""
import argparse
from datetime import datetime, timezone
from pathlib import Path
import subprocess
import sys

REMOTE = 'https://github.com/yuanwil1y/One-OS.git'
# The docs-only audit commit may advance main; require the reviewed firmware tree.
BASE = 'de1afc42b73cb1695ff24bf25c1c77b92fe7958b'
KEEP = {'main', 'research/matter-chip-tool-l2-api'}


def git(*args):
    return subprocess.check_output(['git', *args], text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    manifest = Path(__file__).with_name('branch-cleanup-candidates.tsv')
    candidates = dict(line.split('\t') for line in manifest.read_text().splitlines())
    if KEEP.intersection(candidates) or len(candidates) != 20:
        raise RuntimeError('Unexpected cleanup manifest; aborting.')
    remote = {ref.removeprefix('refs/heads/'): sha for sha, ref in
              (line.split('\t') for line in git('ls-remote', '--heads', REMOTE).splitlines())}
    pending = {}
    for branch, expected in candidates.items():
        actual = remote.get(branch)
        if actual is None:
            print(f'Already absent: {branch}')
        elif actual != expected:
            raise RuntimeError(f'Branch changed since audit: {branch}; re-review required.')
        else:
            pending[branch] = expected
            print(f'Delete candidate: {branch} @ {expected}')
    if not args.apply or not pending:
        print('No changes made.' if not args.apply else 'Nothing left to delete.')
        return
    backup = Path.cwd() / ('One-OS-before-cleanup-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ') + '.git')
    subprocess.run(['git', 'clone', '--mirror', REMOTE, str(backup)], check=True)
    main_sha = git('-C', str(backup), 'rev-parse', 'refs/heads/main')
    if main_sha != remote.get('main'):
        raise RuntimeError('main changed during backup; re-run after review.')
    # Allow only audit docs/tools changes above the reviewed baseline.
    changed = git('-C', str(backup), 'diff', '--name-only', BASE, main_sha).splitlines()
    allowed = {'README.md', 'docs/development-status.md', 'tools/branch-cleanup-candidates.tsv', 'tools/cleanup_remote_branches.py', 'docs/pre-ui-development.md', 'docs/application/nearby-devices-browser-controller.md', 'docs/research/zha-zigpy-l2-api.md', 'firmware/components/esphome_l2/PROVENANCE.md'}
    if set(changed) - allowed:
        raise RuntimeError('main changed beyond audit files; re-review required.')
    for branch, expected in pending.items():
        if git('-C', str(backup), 'rev-parse', 'refs/heads/' + branch) != expected:
            raise RuntimeError(f'Branch changed during backup: {branch}')
    print(f'Full mirror backup: {backup}')
    # Use a normal push URL (not the mirror remote) and explicit per-ref leases.
    leases = [f'--force-with-lease=refs/heads/{branch}:{sha}' for branch, sha in pending.items()]
    refs = [f':refs/heads/{branch}' for branch in pending]
    subprocess.run(['git', 'push', '--atomic', *leases, REMOTE, *refs], check=True)
    remaining = git('ls-remote', '--heads', REMOTE)
    print('Remaining remote branches:\n' + remaining)


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as exc:
        sys.exit(str(exc))
