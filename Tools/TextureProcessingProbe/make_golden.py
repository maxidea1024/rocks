"""Builds and runs the texture processing golden probe (Docs/Design/AssetSystem section 12.18 TP-D4).

Usage: python make_golden.py <Unreal Engine 5.8 clone> [--clang <clang++>] [--work <directory>]

The script cuts function ranges out of TextureCompressorModule.cpp and TextureLODSettings.cpp
(UE 5.8.4, commit f45729c0b6f0 - every range is checked by its first and last line), compiles them with ProbeShim.h
twice and writes the reference build output to Engine/Source/Developer/TextureTools/Tests/Fixtures/TextureProcessingGolden.
The reference build computes transcendental functions with Rocks DeterministicMath and rounds to float (the Rocks
build values); the UE build calls the host
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
LOD_RELATIVE = pathlib.Path('Engine/Source/Runtime/Engine/Private/TextureLODSettings.cpp')
OUTPUT = ROOT / 'Engine/Source/Developer/TextureTools/Tests/Fixtures/TextureProcessingGolden'
EXPECTED_COMMIT = 'f45729c0b6f097038e72b52d366a62baeca3275b'
OUTPUT_FILES = ['KernelGolden.bin', 'MipChainGolden.bin']
DETERMINISTIC_MATH = ROOT / 'Core/Source/Runtime/DeterministicMath'

# 잘라 올 구간이다: (원본, 첫 줄, 마지막 줄, 마지막 줄 뒤 더 가져올 줄 수) — 양 끝을 포함하고
# 내용으로 찾는다(줄 번호에 기대지 않는다). 마지막 줄이 None이면 첫 줄 뒤 처음 나오는 '}' 줄까지다.
RANGES = [
    (TCM_RELATIVE, 'enum EMipGenAddressMode', '\tfloat KernelWeights1D[MaxKernelExtend];', 1),
    (TCM_RELATIVE, 'static float DetermineScaledThreshold(float Threshold, float Scale)', '\treturn AddressMode;', 1),
    (TCM_RELATIVE, 'void ITextureCompressorModule::GenerateMipChain(', '\t\t\tLowLevelTasks::ETaskPriority::BackgroundNormal);', 2),
    (LOD_RELATIVE, 'void UTextureLODSettings::GetMipGenSettings(const UTexture& Texture, TextureMipGenSettings& OutMipGenSettings, float& OutSharpen, uint32& OutKernelSize, bool& bOutDownsampleWithAverage, bool& bOutSharpenWithoutColorShift, bool &bOutBorderColorBlack) const', None, 0),
]


def default_clang() -> pathlib.Path:
    base = pathlib.Path(os.environ.get('LOCALAPPDATA', '')) / 'Rocks/out/toolchains'
    candidates = sorted(base.glob('llvm-*/bin/clang++.exe')) + sorted(base.glob('llvm-*/bin/clang++'))
    if not candidates:
        sys.exit('no clang++ under ' + str(base) + ' (pass --clang)')
    return candidates[-1]


def extract(unreal: pathlib.Path) -> str:
    pieces = []
    cursors = {}
    cache = {}
    for relative, first, last, tail in RANGES:
        if relative not in cache:
            cache[relative] = (unreal / relative).read_text(encoding='utf-8-sig').splitlines()
        lines = cache[relative]
        cursor = cursors.get(relative, 0)
        try:
            start = next(i for i in range(cursor, len(lines)) if lines[i] == first)
            if last is None:
                end = next(i for i in range(start, len(lines)) if lines[i] == '}')
            else:
                end = next(i for i in range(start, len(lines)) if lines[i] == last) + tail
        except StopIteration:
            sys.exit('range not found in ' + str(relative) + ': ' + first[:60])
        pieces.append('#line {} "{}"'.format(start + 1, relative.name))
        pieces.extend(lines[start:end + 1])
        cursors[relative] = end + 1
    return '\n'.join(pieces) + '\n'


def build(clang: pathlib.Path, work: pathlib.Path, ue_libm: bool) -> pathlib.Path:
    name = 'TextureProcessingProbe' + ('Ue' if ue_libm else 'Reference')
    probe = work / (name + ('.exe' if os.name == 'nt' else ''))
    command = [
        str(clang), '-std=c++20', '-O2', '-msse2', '-mno-avx', '-mno-avx2', '-mno-fma', '-ffp-contract=off',
        '-fno-fast-math', '-DNDEBUG', '-DROCKS_PROBE_UE_LIBM=' + ('1' if ue_libm else '0'), '-w',
        '-I', str(work), '-I', str(HERE), str(HERE / 'TextureProcessingProbe.cpp'), '-o', str(probe),
    ]
    if not ue_libm:
        # 기준 판은 Rocks DeterministicMath를 같이 컴파일한다(정의상 Rocks 빌드와 같은 값).
        command += [
            '-DROCKS_DETERMINISTICMATH_API=', '-DROCKS_PLATFORMBASE_API=', '-DROCKS_BASE_API=', '-I', str(DETERMINISTIC_MATH / 'Public'),
            '-I', str(ROOT / 'Core/Source/Runtime/Base/Public'),
            '-I', str(ROOT / 'Core/Source/Runtime/PlatformBase/Public'),
            str(DETERMINISTIC_MATH / 'Private/DeterministicMath.cpp'),
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
    for relative in (TCM_RELATIVE, LOD_RELATIVE):
        if not (unreal / relative).is_file():
            sys.exit(str(relative) + ' not found under ' + str(unreal))

    head = subprocess.run(['git', '-C', str(unreal), 'rev-parse', 'HEAD'], capture_output=True, text=True)
    if head.returncode != 0 or head.stdout.strip() != EXPECTED_COMMIT:
        sys.exit('the UE clone must be at commit ' + EXPECTED_COMMIT + ' (found ' + head.stdout.strip() + ')')

    clang = pathlib.Path(args.clang) if args.clang else default_clang()
    work = pathlib.Path(args.work) if args.work else pathlib.Path(tempfile.mkdtemp(prefix='texture-processing-'))
    work.mkdir(parents=True, exist_ok=True)
    (work / 'TcmExtract.inc').write_text(extract(unreal), encoding='utf-8')

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
