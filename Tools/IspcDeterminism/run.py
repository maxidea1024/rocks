"""ISPC 타깃 간 결정성을 확인한다(Docs/Design/AssetSystem 12.17.6 10).

Usage: python Tools/IspcDeterminism/run.py [--ispc <ispc>] [--clang <clang++>] [--work <directory>]

The script compiles Engine/Source/ThirdParty/IspcTextureCompressor kernel.ispc (with Rocks.patch applied) with the pinned
ISPC once per target of the host architecture (x64: sse2-i32x4, sse4-i32x4, avx1-i32x8, avx2-i32x8; Arm64: neon-i32x4,
neon-i32x8) using the Rocks build arguments (-O2 --opt=disable-fma --math-lib=default, --pic off Windows), links each
object with IspcDeterminismProbe.cpp and ispc_texcomp.cpp, runs the probes and compares their output hashes and dumped
block bytes. On x64 the golden hash of the Rocks target (sse4-i32x4) must also equal the Rocks hash golden read from
EngineTests TextureBlockBptc_OutputHashGolden. Exit code 0 when every check passes, 1 otherwise.
"""

import argparse
import json
import os
import pathlib
import platform
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
LIBRARY = ROOT / 'Engine/Source/ThirdParty/IspcTextureCompressor/IspcTextureCompressor-79ddbc90334f/ispc_texcomp'
GOLDEN_IMAGES = ROOT / 'Engine/Source/Developer/TextureTools/Tests'
GOLDEN_TEST = GOLDEN_IMAGES / 'TextureBlockBptcTests.cpp'
# 호스트 아키텍처마다 비교할 타깃과 Rocks 빌드가 쓰는 타깃이다(Programs/Rocks.Build/Ispc.cs IspcTools.TargetOf).
TARGETS = {
    'x64': (['sse2-i32x4', 'sse4-i32x4', 'avx1-i32x8', 'avx2-i32x8'], 'sse4-i32x4', 'x86-64'),
    'arm64': (['neon-i32x4', 'neon-i32x8'], 'neon-i32x4', 'aarch64'),
}
# Rocks 빌드의 고정 인자다(IspcTools.Arguments — 타깃·arch·target-os만 바꾼다). 서드파티 원본이라 경고는 끈다.
FIXED = ['-O2', '--opt=disable-fma', '--math-lib=default', '--woff']


def host_architecture() -> str:
    machine = platform.machine().lower()
    if machine in ('amd64', 'x86_64'):
        return 'x64'
    if machine in ('arm64', 'aarch64'):
        return 'arm64'
    sys.exit('unsupported host architecture: ' + machine)


def pinned_ispc_version() -> str:
    return json.loads((ROOT / 'Build/Toolchain.json').read_text(encoding='utf-8'))['ispc']['version']


# ISPC를 Rocks 빌드와 같은 순서로 찾는다(IspcTools.DiscoverAsync — ROCKS_ISPC, Setup 기본 위치, 저장소 out/toolchains).
def find_ispc() -> pathlib.Path:
    name = 'ispc.exe' if os.name == 'nt' else 'ispc'
    directory = 'ispc-' + pinned_ispc_version()
    if os.environ.get('ROCKS_ISPC'):
        return pathlib.Path(os.environ['ROCKS_ISPC'])
    candidates = []
    if os.name == 'nt' and os.environ.get('LOCALAPPDATA'):
        candidates.append(pathlib.Path(os.environ['LOCALAPPDATA']) / 'Rocks/toolchains' / directory / name)
    if sys.platform == 'darwin':
        candidates.append(pathlib.Path.home() / 'Library/Application Support/Rocks/toolchains' / directory / name)
    candidates.append(ROOT / 'out/toolchains' / directory / name)
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    sys.exit('ISPC ' + pinned_ispc_version() + ' not found (set ROCKS_ISPC or run Build/InstallIspc.ps1)')


def find_clang() -> pathlib.Path:
    if os.environ.get('ROCKS_CLANGXX'):
        return pathlib.Path(os.environ['ROCKS_CLANGXX'])
    if os.name == 'nt':
        base = pathlib.Path(os.environ.get('LOCALAPPDATA', '')) / 'Rocks'
        candidates = sorted(base.glob('toolchains/llvm-*/bin/clang++.exe')) + sorted(base.glob('out/toolchains/llvm-*/bin/clang++.exe'))
        if candidates:
            return candidates[-1]
        sys.exit('no clang++ under ' + str(base) + ' (set ROCKS_CLANGXX or pass --clang)')
    return pathlib.Path('clang++')


def rocks_golden() -> str:
    match = re.search(r'hash == 0x([0-9a-f]{16})ull', GOLDEN_TEST.read_text(encoding='utf-8'))
    if match is None:
        sys.exit('no hash golden in ' + str(GOLDEN_TEST))
    return match.group(1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--ispc')
    parser.add_argument('--clang')
    parser.add_argument('--work')
    args = parser.parse_args()
    ispc = pathlib.Path(args.ispc) if args.ispc else find_ispc()
    clang = pathlib.Path(args.clang) if args.clang else find_clang()
    work = pathlib.Path(args.work) if args.work else pathlib.Path(tempfile.mkdtemp(prefix='ispc-determinism-'))
    work.mkdir(parents=True, exist_ok=True)
    architecture = host_architecture()
    targets, rocks_target, arch = TARGETS[architecture]
    target_os = 'windows' if os.name == 'nt' else 'macos'
    extra = [] if os.name == 'nt' else ['--pic']
    print('work directory:', work)
    results = {}
    for target in targets:
        directory = work / target
        directory.mkdir(exist_ok=True)
        obj = directory / ('kernel.obj' if os.name == 'nt' else 'kernel.o')
        subprocess.run([str(ispc), *FIXED, *extra, '--target=' + target, '--arch=' + arch, '--target-os=' + target_os,
                        str(LIBRARY / 'kernel.ispc'), '-o', str(obj), '-h', str(directory / 'kernel_ispc.h')], check=True)
        probe = directory / ('IspcDeterminismProbe.exe' if os.name == 'nt' else 'IspcDeterminismProbe')
        subprocess.run([str(clang), '-std=c++17', '-O2', '-ffp-contract=off', '-fno-fast-math', '-w',
                        '-I', str(directory), '-I', str(LIBRARY), '-I', str(GOLDEN_IMAGES),
                        str(pathlib.Path(__file__).resolve().parent / 'IspcDeterminismProbe.cpp'), str(LIBRARY / 'ispc_texcomp.cpp'),
                        str(obj), '-o', str(probe)], check=True)
        dump = directory / 'blocks.bin'
        completed = subprocess.run([str(probe), str(dump)], check=True, capture_output=True, text=True)
        hashes = dict(line.split() for line in completed.stdout.strip().splitlines())
        results[target] = (hashes, dump.read_bytes())
        print(f'{target:12} golden {hashes["golden"]}  full {hashes["full"]}')

    ok = True
    reference_hashes, reference_blocks = results[rocks_target]
    for target, (hashes, blocks) in results.items():
        if hashes != reference_hashes or blocks != reference_blocks:
            first = next((i for i in range(min(len(blocks), len(reference_blocks))) if blocks[i] != reference_blocks[i]),
                         min(len(blocks), len(reference_blocks)))
            print(f'DIFFERENT {target}: first differing byte {first} (block {first // 16}) of the dump')
            ok = False
    if architecture == 'x64':
        golden = rocks_golden()
        if reference_hashes['golden'] != golden:
            print(f'{rocks_target} golden hash {reference_hashes["golden"]} differs from the Rocks golden {golden}')
            ok = False
    else:
        print('Arm64: record the golden hash of', rocks_target, 'as the Arm64 value (the test asserts x64 only)')
    print('PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
