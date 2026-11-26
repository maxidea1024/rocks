#pragma once

#include <AssetCommandlets/AssetCommandletSupport.h>
#include <Core/Commandlet/Commandlet.h>
#include <UnrealExchange/ExchangeImportRun.h>

// asset-import-unreal 명령이다(Docs/Design/UnrealExchange 10.2절, 10.5절 9·10번). Commandlet
// 호스트가 등록하고 EngineTests가 시험 임포터로 프로세스 안에서 부른다.
//
//   asset-import-unreal <묶음 디렉터리> [--root=/Game/Unreal] [--only=<objectPath>]...
//                       [--mesh-normals=unreal|rocks] [--dry-run]
//                       [--project=<경로>] [--wait=<초>]
//
// 사용 예(호스트 등록):
//   UnrealExchangeCommandContext unreal{&assetContext, {}};
//   registry.Register({AssetImportUnrealName, "Import an Unreal exchange bundle.",
//                      &RunAssetImportUnrealCommand, &unreal});

namespace rocks {

// 명령 이름이다.
inline constexpr StringView AssetImportUnrealName = "asset-import-unreal";

// 종류별 임포터 등록 hook이다(세션의 서비스를 연 뒤 부른다 — A2f의 AssetImporterSetup과 같은
// 모양). 등록한 임포터는 명령이 끝날 때까지 살아 있어야 한다. 실패하면 그 오류로 명령이 실패한다.
// 제품은 지금 등록하는 것이 없다(UX2b 이후), 시험은 시험 임포터를 준다.
struct ExchangeKindSetup {
    Result<void> (*registerKinds)(void* state, ExchangeKindTable& table) noexcept = nullptr;
    void* state = nullptr;
};

// 명령 문맥이다: 에셋 명령 문맥(작업 메모리·모듈 로더·타입 모듈·A2f 임포터 등록)과 종류별 임포터
// 등록.
struct UnrealExchangeCommandContext {
    const commandlet_host::AssetCommandContext* assets = nullptr;
    ExchangeKindSetup kinds{};
};

// 명령을 실행한다(context는 UnrealExchangeCommandContext). 종료 코드: 실패한 에셋이 없으면 0,
// 있거나 묶음 읽기·계획·세션 열기가 실패하면 1, 사용법 오류 2. 반환 오류: 출력 쓰기 실패, 할당
// 실패.
ROCKS_UNREALEXCHANGE_API Result<int32>
RunAssetImportUnrealCommand(void* context, Span<const StringView> args,
                            CommandletOutput& output) noexcept;

} // namespace rocks
