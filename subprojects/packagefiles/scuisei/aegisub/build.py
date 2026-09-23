#!/usr/bin/env python3
"""Build the pinned scuisei detector, without its CLI or FFmpeg dependencies."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('target')
    parser.add_argument('crt')
    args = parser.parse_args()
    cargo = shutil.which('cargo')
    if not cargo:
        candidate = Path(os.environ.get('CARGO_HOME', Path.home() / '.cargo')) / 'bin' / ('cargo.exe' if os.name == 'nt' else 'cargo')
        if candidate.is_file():
            cargo = str(candidate)
    if not cargo:
        raise SystemExit('scuisei requires Rust/Cargo 1.93 or newer. Install the minimal Rust toolchain from https://rustup.rs.')
    output = args.output.resolve()
    target_dir = output.parent / 'cargo-target'
    env = os.environ.copy()
    env['PATH'] = str(Path(cargo).parent) + os.pathsep + env.get('PATH', '')
    flags = env.get('RUSTFLAGS', '')
    if args.target.endswith('msvc'):
        flags += ' -C target-feature=' + ('+crt-static' if args.crt in ('mt', 'mtd', 'static_from_buildtype') else '-crt-static')
    env['RUSTFLAGS'] = flags.strip()
    subprocess.run([cargo, 'build', '--locked', '--release', '--lib',
                    '--manifest-path', str(args.manifest.resolve()),
                    '--target-dir', str(target_dir), '--target', args.target], env=env, check=True)
    filename = 'aegisub_scuisei.lib' if args.target.endswith('msvc') else 'libaegisub_scuisei.a'
    shutil.copyfile(target_dir / args.target / 'release' / filename, output)


if __name__ == '__main__':
    main()
