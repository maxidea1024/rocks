#pragma once

#include <ModuleLoader/ModuleReport.h>

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Module/ModuleEntry.h>
#include <Core/Text/StringView.h>
#include <Platform/DynamicLibrary.h>

// Modular 모듈 manifest의 해석과 바이너리 준비다(Docs/Design/ModuleLoader.ko.md 3.5·3.13·3.15절).
// 로더(ModuleLoader::TryCreateFromManifest)와 시험만 쓴다. 공개 API가 아니다.
namespace rocks::detail {

// 실행 파일 manifest와 그 플러그인 manifest들에서 만든 모듈 표와 그 저장소다. 로더가 소유하고
// 로더와 같이 산다(행 배열·이름이 정적 수명이 아니다). 행은 order 순서다(3.15절 — Monolithic 표
// 순서). 소멸하면 연 라이브러리를 적재 역순으로 닫는다 — 그 전에 로더가 모든 모듈을 내리고 수명
// 객체를 파괴해야 한다.
struct ROCKS_MODULELOADER_API ModuleManifestData {
    explicit ModuleManifestData(Allocator& allocator) noexcept;
    ~ModuleManifestData() noexcept;
    ModuleManifestData(const ModuleManifestData&) = delete;
    ModuleManifestData& operator=(const ModuleManifestData&) = delete;

    // 모듈 표 행이다. entry 칸은 비어 있고 진입 기술은 entries의 같은 색인에 있다(Prepare 뒤).
    Array<ModuleTableRow> rows;
    // 행마다 Prepare가 얻은 진입 기술이다.
    Array<const ModuleEntry*> entries;
    // 행마다 manifest 폴더 기준 파일 이름·기대 크기·호스트 모듈 행인지다.
    Array<const char*> files;
    Array<uint64> sizes;
    Array<bool> host;
    // 행마다 그 행을 실은 manifest의 폴더(행 파일을 여기서 연다)와 플러그인 이름(실행 파일
    // manifest 행은 null)이다.
    Array<const char*> directories;
    Array<const char*> plugins;
    // 이름·파일 이름·폴더·플러그인 이름 문자열(NUL 종료)을 이어 둔 저장소다. 한 번에 정확한
    // 크기로 잡아 주소가 고정된다.
    Array<char> text;
    // 모든 행의 의존 이름 포인터를 이어 둔 배열이다(text 안의 앞 행 이름을 가리킨다).
    Array<const char*> dependencyNames;
    // Prepare가 연 라이브러리와 그 진입점 lease다(적재 순서 — 모든 행을 싣으므로 행 색인과 같다).
    Array<platform::DynamicLibrary> libraries;
    Array<platform::DynamicLibraryLease> leases;
};

// 해석할 manifest 하나다. ParseModuleManifests의 첫 원소는 실행 파일 manifest이고, 나머지는 그
// plugins 목록 순서의 플러그인 manifest다.
struct ModuleManifestText {
    // manifest 본문이다.
    StringView text;
    // manifest 폴더(절대 경로)다. 그 manifest 행의 파일을 이 폴더에서 연다.
    StringView directory;
    // 플러그인 manifest면 플러그인 이름, 실행 파일 manifest면 빈 값이다.
    StringView plugin;
};

// 실행 파일 manifest의 plugins 목록이다(ListModuleManifestPlugins가 채운다). 이름과 상대 경로를
// NUL로 이어 저장한다.
struct ROCKS_MODULELOADER_API ModuleManifestPlugins {
    explicit ModuleManifestPlugins(Allocator& allocator) noexcept;

    usize Count() const noexcept {
        return names.Size();
    }

    StringView Name(usize index) const noexcept {
        return StringView(text.Data() + names[index]);
    }

    // 실행 파일 manifest 폴더 기준 상대 경로("../Plugins/…/Binaries/<Platform>/<파일 이름>")다.
    StringView Path(usize index) const noexcept {
        return StringView(text.Data() + paths[index]);
    }

    Array<char> text;
    Array<usize> names;
    Array<usize> paths;
};

// 실행 파일 manifest 본문의 머리(JSON·최상위 키·schema·buildId·engineVersion·
// toolchainFingerprint)를 검사하고 plugins 목록을 엄격하게 해석한다(3.15절 — 플러그인 manifest를
// 열기 전에): 항목 키는 정확히 name·manifest, 이름은 C 식별자(중복 없음), 경로는
// "../Plugins/<조각>/ …/Binaries/<조각>/<fileName>" 모양(조각은 비지 않고 .·..·\·:이 없다)이다.
// fileName은 실행 파일 manifest 파일 이름이다. 행 검사는 ParseModuleManifests가 한다. 실패하면
// false이고 report에 Incompatible(단계 "Manifest")과 이유를 쓴다.
ROCKS_MODULELOADER_API bool ListModuleManifestPlugins(StringView text, StringView fileName,
                                                      ModuleManifestPlugins& plugins,
                                                      ModuleLoadReport& report) noexcept;

// manifest들을 엄격하게 해석해 data를 채운다(OS 적재 없음). 실행 파일 manifest의 plugins는
// manifests[1..]과 개수·이름이 같아야 하고, 플러그인 manifest는 호스트 행이 없고 plugins가 비며
// buildId·engineVersion·toolchainFingerprint가 실행 파일 manifest와 같아야 한다. 행은 order로
// 정렬하고(manifest 안에서는 order가 커지는 순서여야 한다), 이름·order는 manifest 사이에서도
// 하나이며 의존은 order가 더 작은 행이어야 한다. 실패하면 false이고 report에 Incompatible(단계
// "Manifest")과 키 경로·JSON 위치를 담은 문구를 쓴다(플러그인 manifest의 실패면 requested가
// 플러그인 이름이다). 할당 실패는 false와 OutOfMemory 문구다(code Incompatible, error OutOfMemory).
ROCKS_MODULELOADER_API bool ParseModuleManifests(Span<const ModuleManifestText> manifests,
                                                 ModuleManifestData& data,
                                                 ModuleLoadReport& report) noexcept;

// 플러그인 manifest가 없는 실행 파일 manifest 하나를 해석한다(폴더는 빈 값 — 시험용).
ROCKS_MODULELOADER_API bool ParseModuleManifest(StringView text, ModuleManifestData& data,
                                                ModuleLoadReport& report) noexcept;

// 해석한 행의 바이너리를 표 순서(의존 순서)로 준비한다(MD22 Prepare): 파일 크기 대조, OS 적재,
// export 진입점 RocksModuleEntry 조회와 호출, 진입 기술 대조. 호스트 행도 다른 행과 같이 싣는다.
// 행 파일은 그 행의 manifest 폴더에서 연다. 다른 폴더의 import는 먼저 연 의존 라이브러리로 풀린다
// (3.15절). 실패하면 false이고 report에 NotFound·Incompatible·OsLoadFailed·EntryMissing을 담으며
// (플러그인 행이면 requested가 플러그인 이름) 이미 연 라이브러리를 역순으로 닫는다.
bool PrepareModuleBinaries(ModuleManifestData& data, ModuleLoadReport& report) noexcept;

// 연 라이브러리를 적재 역순으로 닫는다(lease를 먼저 놓는다).
void CloseModuleBinaries(ModuleManifestData& data) noexcept;

} // namespace rocks::detail
