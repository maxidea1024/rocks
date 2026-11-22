#pragma once

#include <AssetRuntime/AssetManager.h>
#include <Core/Containers/Array.h>
#include <Platform/File.h>
#include <Config/CVar.h>
#include <Object/ObjectRegistryOptions.h>

// EngineAssets 모듈 안에서 함께 쓰는 CVar와 예산 읽기다(Docs/Design/AssetSystem.ko.md 11.8절 3).
namespace rocks::engine_assets_detail {

extern BootCVar<String> CookedRootVar;
extern BootCVar<String> RuntimeProfileVar;
extern BootCVar<int32> LoaderWorkersVar;
extern BootCVar<int32> IoWorkersVar;
extern BootCVar<int64> MaxPackagesInFlightVar;
extern BootCVar<int64> MaxTempBytesVar;
extern CVar<double> PumpMillisecondsVar;
extern CVar<int64> PumpMaxUnitsVar;
extern CVar<int64> PumpMaxPostLoadsVar;
extern CVar<int64> PumpMaxDiscardsVar;
extern CVar<double> GcMillisecondsVar;
extern CVar<int64> GcMaxObjectsVar;
extern CVar<double> GcRetrySecondsVar;

// 밀리초 CVar 값을 ns로 바꾼다(소수 부분은 버린다).
uint64 MillisecondsToNanoseconds(double milliseconds) noexcept;
// 0이면 unlimited, 아니면 그 값이다(CVar 범위가 uint32 안이다).
uint32 CountOrUnlimited(int64 value, uint32 unlimited) noexcept;
// 지금 설정 snapshot의 Pump·GC 예산이다(게임 스레드 — 프레임마다 읽는다).
ROCKS_ENGINEASSETS_API AssetManagerPumpBudget ReadPumpBudget() noexcept;
ROCKS_ENGINEASSETS_API GcBudget ReadGcBudget() noexcept;

// files에서 path 파일 전체를 out으로 읽는다(매니페스트 — 11.8절 4). 오류: 없으면 NotFound, 크기가
// maxBytes를 넘으면 CapacityExceeded, 파일 오류는 FileErrorCode 대응, 할당 실패. 시험은 고장 난
// FileSystem을 끼운다.
ROCKS_ENGINEASSETS_API Result<void> ReadManifestFile(platform::FileSystem& files, StringView path,
                                                     uint64 maxBytes, Array<uint8>& out) noexcept;

} // namespace rocks::engine_assets_detail
