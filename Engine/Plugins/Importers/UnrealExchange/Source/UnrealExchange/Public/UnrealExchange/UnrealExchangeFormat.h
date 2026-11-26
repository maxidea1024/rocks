#pragma once

#include <Base/NumericLimits.h>
#include <Base/Types.h>
#include <Core/Text/NameComponent.h>
#include <Core/Text/StringView.h>

// Unreal 교환 묶음 형식의 공통 상수다(Docs/Design/UnrealExchange 3절, 9절, 10.4절). 묶음은 UE
// 쪽 RocksExport 플러그인이 쓰고 이 플러그인의 리더(ExchangeBundle.h)가 읽는다.

namespace rocks {

// 이 리더가 받는 bundle.json의 formatVersion이다(3.4절). 다른 값은 UnsupportedBundleVersion
// 오류다(하위호환 읽기 없음).
inline constexpr uint32 UnrealExchangeBundleFormatVersion = 1;

// 경로 대응(9.1절): Rocks 논리 경로 구성 요소의 byte 상한(AssetSystem 4.2절과 같다), 정리한 이름에
// 붙이는 해시 접미의 hex 글자 수, 대응 결과 논리 경로 전체의 byte 상한.
inline constexpr usize UnrealPathComponentMaxBytes = MaxNameComponentBytes;
inline constexpr usize UnrealPathHashChars = 8;
inline constexpr usize UnrealPathMaxBytes = 256;

// 임포트 순서(9.3절)의 단위 깊이 상한이다. 넘는 단위는 UnsupportedCycle로 실패한다(조작된 묶음
// 방어).
inline constexpr uint32 UnrealImportMaxDepth = 256;

// UE 객체 경로의 byte 상한이다(UE NAME_SIZE와 같은 값, 10.4절 7번).
inline constexpr usize UnrealObjectPathMaxBytes = 1024;

// 리더 상한(10.4절 16번). JSON 파일 하나, 묶음의 JSON 파일(bundle.json + 에셋 JSON) 크기 합,
// blob 하나, 묶음의 에셋 수, redirect 수, 에셋 하나의 의존 수, 묶음 하나가 담는 진단 수다. 진단은
// 상한을 넘으면 개수만 센다. JSON 합 상한은 리더가 문서를 모두 메모리에 두기 때문이다(10.4절 18번).
inline constexpr uint64 UnrealExchangeMaxJsonBytes = 64ull << 20;
inline constexpr uint64 UnrealExchangeMaxBundleJsonBytes = 1ull << 30;
inline constexpr uint64 UnrealExchangeMaxBlobBytes = 4ull << 30;
inline constexpr uint32 UnrealExchangeMaxAssets = 1u << 20;
inline constexpr uint32 UnrealExchangeMaxRedirects = 1u << 20;
inline constexpr uint32 UnrealExchangeMaxDependencies = 65536;
inline constexpr uint32 UnrealExchangeMaxDiagnostics = 4096;

// blob을 검증할 때 한 번에 읽는 조각 크기다(파일 전체를 메모리에 올리지 않는다, 10.4절 12번).
inline constexpr usize UnrealExchangeBlobChunkBytes = 64 * 1024;

// 임포트한 에셋의 색인 태그 키다(9.2절 — 지금 UE objectPath와 묶음 redirect로 알게 된 옛 경로들,
// 기존 에셋 조회가 찾는다, 10.5절 4번). 키 접두는 교환 임포터들의 태그 접두 UnrealExchangeTagPrefix다
// (AssetSystem 7.1절 21 A2f5).
inline constexpr StringView UnrealExchangeObjectPathTag = "UnrealExchange.ObjectPath";
inline constexpr StringView UnrealExchangeFormerPathsTag = "UnrealExchange.FormerPaths";

// 교환 A2f 임포터의 원본 역할·가변 원본 역할 접두(10.6절 2번 — `blob:<64자 hex>`)와 태그 접두(7번 —
// AssetSystem 7.1절 21 A2f5 tagPrefix, 태그 키 UnrealExchangeObjectPathTag의 앞부분)다.
inline constexpr StringView UnrealExchangeAssetRole = "asset";
inline constexpr StringView UnrealExchangeBlobRolePrefix = "blob:";
inline constexpr StringView UnrealExchangeTagPrefix = "UnrealExchange";

// 에셋·의존 번호가 없음을 나타내는 값이다.
inline constexpr uint32 ExchangeNoIndex = NumericLimits<uint32>::Max();

} // namespace rocks
