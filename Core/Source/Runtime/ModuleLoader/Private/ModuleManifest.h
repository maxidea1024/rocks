#pragma once

#include <ModuleLoader/ModuleReport.h>

#include <Core/Containers/Array.h>
#include <Core/Module/ModuleEntry.h>
#include <Core/Text/StringView.h>
#include <Platform/DynamicLibrary.h>

// Modular 모듈 manifest의 해석과 바이너리 준비다(Docs/Design/ModuleLoader.ko.md 3.5·3.13절). 로더
// (ModuleLoader::TryCreateFromManifest)와 시험만 쓴다. 공개 API가 아니다.
namespace rocks::detail {

// manifest 하나에서 만든 모듈 표와 그 저장소다. 로더가 소유하고 로더와 같이 산다(행 배열·이름이
// 정적 수명이 아니다). 소멸하면 연 라이브러리를 적재 역순으로 닫는다 — 그 전에 로더가 모든 모듈을
// 내리고 수명 객체를 파괴해야 한다.
struct ModuleManifestData {
    explicit ModuleManifestData(Allocator& allocator) noexcept;
    ~ModuleManifestData() noexcept;
    ModuleManifestData(const ModuleManifestData&) = delete;
    ModuleManifestData& operator=(const ModuleManifestData&) = delete;

    // 모듈 표 행이다. entry 칸은 비어 있고 진입 기술은 entries의 같은 색인에 있다(Prepare 뒤).
    Array<ModuleTableRow> rows;
    // 행마다 Prepare가 얻은 진입 기술이다.
    Array<const ModuleEntry*> entries;
    // 행마다 manifest 폴더 기준 파일 이름·기대 크기·실행 파일 행인지다.
    Array<const char*> files;
    Array<uint64> sizes;
    Array<bool> executable;
    // 이름·파일 이름 문자열(NUL 종료)을 이어 둔 저장소다. 한 번에 정확한 크기로 잡아 주소가
    // 고정된다.
    Array<char> text;
    // 모든 행의 의존 이름 포인터를 이어 둔 배열이다(text 안의 앞 행 이름을 가리킨다).
    Array<const char*> dependencyNames;
    // Prepare가 연 라이브러리와 그 진입점 lease다(적재 순서).
    Array<platform::DynamicLibrary> libraries;
    Array<platform::DynamicLibraryLease> leases;
};

// manifest 본문을 엄격하게 해석해 data를 채운다(OS 적재 없음). 실패하면 false이고 report에
// Incompatible(단계 "Manifest")과 키 경로·JSON 위치를 담은 문구를 쓴다. 할당 실패는 false와
// OutOfMemory 문구다(code Incompatible, error OutOfMemory).
bool ParseModuleManifest(StringView text, ModuleManifestData& data,
                         ModuleLoadReport& report) noexcept;

// 해석한 행의 바이너리를 의존 순서로 준비한다(MD22 Prepare): 파일 크기 대조, OS 적재, export
// 진입점 RocksModuleEntry 조회와 호출, 진입 기술 대조. 실행 파일 행은 executableEntry를 쓴다
// (크기만 대조). directory는 manifest가 있는 폴더의 절대 경로다. 실패하면 false이고 report에
// NotFound·Incompatible·OsLoadFailed·EntryMissing을 담으며 이미 연 라이브러리를 역순으로 닫는다.
// 실행 파일 행이 있는데 executableEntry가 null이면 Incompatible(단계 "Manifest")이다.
bool PrepareModuleBinaries(StringView directory, ModuleManifestData& data,
                           ModuleEntryFunction executableEntry, ModuleLoadReport& report) noexcept;

// 연 라이브러리를 적재 역순으로 닫는다(lease를 먼저 놓는다).
void CloseModuleBinaries(ModuleManifestData& data) noexcept;

} // namespace rocks::detail
