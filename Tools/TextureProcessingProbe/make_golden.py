"""Builds and runs the texture processing golden probe (Docs/Design/AssetSystem section 12.18 TP-D4).

Usage: python make_golden.py <Unreal Engine 5.8 clone> [--clang <clang++>] [--work <directory>]

The script cuts function ranges out of Engine/Source/Developer/TextureCompressor/Private/TextureCompressorModule.cpp
(UE 5.8.4, commit f45729c0b6f0 - every range is checked by its first and last line), compiles them with ProbeShim.h
twice and writes the reference build output to Engine/Source/Developer/TextureTools/Tests/Fixtures/TextureProcessingGolden.
The reference build rounds double transcendental results to float (the Rocks build values); the UE build calls the host
float libm like UE FMath. The script prints how many floats differ between the two builds and the largest ulp distance.
UE sources are not copied into the repository. Without --clang the script uses the Rocks LLVM toolchain under
%LOCALAPPDATA%/Rocks/out/toolchains.
"""

import argparse
import os
import pathlib
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
HERE = pathlib.Path(__file__).resolve().parent
TCM_RELATIVE = pathlib.Path('Engine/Source/Developer/TextureCompressor/Private/TextureCompressorModule.cpp')
OUTPUT = ROOT / 'Engine/Source/Developer/TextureTools/Tests/Fixtures/TextureProcessingGolden'
EXPECTED_COMMIT = 'f45729c0b6f097038e72b52d366a62baeca3275b'
OUTPUT_FILES = ['KernelGolden.bin']

# 잘라 올 구간이다: (첫 줄, 마지막 줄) — 양 끝을 포함하고 내용으로 찾는다(줄 번호에 기대지 않는다).
RANGES = [
    ('enum EMipGenAddressMode', '\tfloat KernelWeights1D[MaxKernelExtend];'),
]
# 마지막 줄 뒤에 더 가져올 줄 수다(클래스를 닫는 '};').
RANGE_TAILS = [1]


def default_clang() -> pathlib.Path:
    base = pathlib.Path(os.environ.get('LOCALAPPDATA', '')) / 'Rocks/out/toolchains'
    candidates = sorted(base.glob('llvm-*/bin/clang++.exe')) + sorted(base.glob('llvm-*/bin/clang++'))
    if not candidates:
        sys.exit('no clang++ under ' + str(base) + ' (pass --clang)')
    return candidates[-1]


def extract(tcm: pathlib.Path) -> str:
    lines = tcm.read_text(encoding='utf-8-sig').splitlines()
    pieces = []
    cursor = 0
    for (first, last), tail in zip(RANGES, RANGE_TAILS):
        try:
            start = next(i for i in range(cursor, len(lines)) if lines[i] == first)
            end = next(i for i in range(start, len(lines)) if lines[i] == last) + tail
        except StopIteration:
            sys.exit('range not found in ' + str(tcm) + ': ' + first)
        pieces.append('#line {} "TextureCompressorModule.cpp"'.format(start + 1))
        pieces.extend(lines[start:end + 1])
        cursor = end + 1
    return '\n'.join(pieces) + '\n'


def build(clang: pathlib.Path, work: pathlib.Path, ue_libm: bool) -> pathlib.Path:
    name = 'TextureProcessingProbe' + ('Ue' if ue_libm else 'Reference')
    probe = work / (name + ('.exe' if os.name == 'nt' else ''))
    command = [
        str(clang), '-std=c++17', '-O2', '-msse2', '-mno-avx', '-mno-avx2', '-mno-fma', '-ffp-contract=off',
        '-fno-fast-math', '-DNDEBUG', '-DROCKS_PROBE_UE_LIBM=' + ('1' if ue_libm else '0'), '-w',
        '-I', str(work), '-I', str(HERE), str(HERE / 'TextureProcessingProbe.cpp'), '-o', str(probe),
    ]
    subprocess.run(command, check=True)
    return probe


def float_distance(a: bytes, b: bytes) -> tuple:
    # 같은 배치의 두 파일에서 4 byte 단위로 float 차이를 센다(머리·정수 칸은 두 판이 같다).
    count = 0
    largest = 0
    for offset in range(0, min(len(a), len(b)) - 3, 4):
        x, = struct.unpack_from('<i', a, offset)
        y, = struct.unpack_from('<i', b, offset)
        if x != y:
            count += 1
            # 부호 있는 정수 순서로 바꾼 거리(같은 부호 float의 ulp 거리)
            to_ordered = lambda v: v if v >= 0 else -(v & 0x7FFFFFFF)
            largest = max(largest, abs(to_ordered(x) - to_ordered(y)))
    return count, largest


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('unreal')
    parser.add_argument('--clang')
    parser.add_argument('--work')
    args = parser.parse_args()

    unreal = pathlib.Path(args.unreal)
    tcm = unreal / TCM_RELATIVE
    if not tcm.is_file():
        sys.exit('TextureCompressorModule.cpp not found under ' + str(unreal))

    head = subprocess.run(['git', '-C', str(unreal), 'rev-parse', 'HEAD'], capture_output=True, text=True)
    if head.returncode != 0 or head.stdout.strip() != EXPECTED_COMMIT:
        sys.exit('the UE clone must be at commit ' + EXPECTED_COMMIT + ' (found ' + head.stdout.strip() + ')')

    clang = pathlib.Path(args.clang) if args.clang else default_clang()
    work = pathlib.Path(args.work) if args.work else pathlib.Path(tempfile.mkdtemp(prefix='texture-processing-'))
    work.mkdir(parents=True, exist_ok=True)
    (work / 'TcmExtract.inc').write_text(extract(tcm), encoding='utf-8')

    reference = build(clang, work, False)
    ue = build(clang, work, True)
    OUTPUT.mkdir(parents=True, exist_ok=True)
    ue_output = work / 'ue'
    ue_output.mkdir(exist_ok=True)
    subprocess.run([str(reference), str(OUTPUT)], check=True)
    subprocess.run([str(ue), str(ue_output)], check=True)
    for name in OUTPUT_FILES:
        count, largest = float_distance((OUTPUT / name).read_bytes(), (ue_output / name).read_bytes())
        print('{}: {} floats differ from the UE libm build (largest distance {} ulp)'.format(name, count, largest))
    print('golden:', OUTPUT)
    return 0


if __name__ == '__main__':
    sys.exit(main())
