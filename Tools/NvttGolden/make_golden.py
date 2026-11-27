"""Builds and runs the NVTT 2.0.8 golden probe (Docs/Design/AssetSystem section 12.17.4 item 3).

Usage: python make_golden.py <Unreal Engine 5.8 clone> [--clang <clang++>] [--work <directory>]

The probe compiles the NVTT sources from the UE clone with the UE non-AVX2 settings (SSE2, no AVX2, no FMA) and writes
Engine/Source/Developer/TextureTools/Tests/Fixtures/NvttGolden/NvttBlockGolden.bin. Without --clang the script uses the Rocks
LLVM toolchain under %LOCALAPPDATA%/Rocks/out/toolchains.
"""

import argparse
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
NVTT_RELATIVE = pathlib.Path('Engine/Source/ThirdParty/nvTextureTools/nvTextureTools-2.0.8/src')
OUTPUT = ROOT / 'Engine/Source/Developer/TextureTools/Tests/Fixtures/NvttGolden/NvttBlockGolden.bin'


def default_clang() -> pathlib.Path:
    base = pathlib.Path(os.environ.get('LOCALAPPDATA', '')) / 'Rocks/out/toolchains'
    candidates = sorted(base.glob('llvm-*/bin/clang++.exe')) + sorted(base.glob('llvm-*/bin/clang++'))
    if not candidates:
        sys.exit('no clang++ under ' + str(base) + ' (pass --clang)')
    return candidates[-1]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('unreal')
    parser.add_argument('--clang')
    parser.add_argument('--work')
    args = parser.parse_args()

    nvtt = pathlib.Path(args.unreal) / NVTT_RELATIVE
    if not (nvtt / 'src/nvtt/squish/weightedclusterfit.cpp').is_file():
        sys.exit('NVTT 2.0.8 sources not found under ' + str(nvtt))

    clang = pathlib.Path(args.clang) if args.clang else default_clang()
    work = pathlib.Path(args.work) if args.work else pathlib.Path(tempfile.mkdtemp(prefix='nvtt-golden-'))
    work.mkdir(parents=True, exist_ok=True)
    probe = work / ('NvttGoldenProbe.exe' if os.name == 'nt' else 'NvttGoldenProbe')
    command = [
        str(clang), '-std=c++17', '-O2', '-msse2', '-mno-avx', '-mno-avx2', '-mno-fma', '-ffp-contract=off',
        '-fno-fast-math', '-DNDEBUG', '-w',
        '-I', str(nvtt / 'src'), '-I', str(nvtt / 'src/nvtt/squish'), '-I', str(nvtt / 'project/vc14'),
        str(pathlib.Path(__file__).resolve().parent / 'NvttGoldenProbe.cpp'),
        str(pathlib.Path(__file__).resolve().parent / 'NvttGoldenOptimal.cpp'), '-o', str(probe),
    ]
    subprocess.run(command, check=True)
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(probe), str(OUTPUT)], check=True)
    print('golden:', OUTPUT)
    return 0


if __name__ == '__main__':
    sys.exit(main())
