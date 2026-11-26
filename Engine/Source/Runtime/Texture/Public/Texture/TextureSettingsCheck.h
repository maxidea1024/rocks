#pragma once

#include <AssetRuntime/AssetDiagnostic.h>
#include <Core/Containers/Array.h>
#include <Object/PropertyPath.h>

// 텍스처 설정 판정이다(Docs/Design/AssetSystem 12.8절 판정): 계약 설정(12.4.1절 — 스키마에 있지만
// 아직 구현하지 않은 값)과 숨김 값(InvalidSetting)을 찾는다. 텍스처 빌드(A5i)가 빌드 전에 부르고
// 쿠킹은 그 결과로 실패한다. 임포터·asset-set은 저장을 막지 않는다(UE와 같다 — 빌드에서 거부).
namespace rocks {

class ObjectRegistry;
struct ObjectAccessContext;

// 판정 이슈 하나다: 진단 코드(UnsupportedSetting·InvalidSetting)와 그 설정 경로(Texture 스키마 —
// 최상위 필드 또는 struct 멤버).
struct TextureSettingIssue {
    AssetDiagnosticCode code = AssetDiagnosticCode::UnsupportedSetting;
    PropertyPathBuffer path;
};

// texture(Texture 파생 객체)의 설정을 판정해 이슈를 out에 쓴다(경로 순 — 스키마 필드 순 → 멤버
// 순, 결정적).
// - 계약 필드가 기본값과 다르면 UnsupportedSetting이다(FindChangedFields — 차이 저장과 같은 같음).
//   영향 조건이 거짓인 필드는 보지 않는다: chromaKeyThreshold·chromaKeyColor(chromaKeyTexture일
//   때), alphaCoverageThresholds(doScaleMipsForAlphaCoverage일 때), compositeTextureMode·
//   compositePower(compositeTexture가 non-null일 때), 색도 좌표
//   넷·chromaticAdaptationMethod(colorSpace = Custom일 때), VT
//   우선순위·useVirtualTextureStreamingPriority·virtualTexturePrefetchMips
//   (virtualTextureStreaming일 때). oodleTextureSdkVersion·useNewMipFilter·normalizeNormals는
//   기록만 한다(판정 없음).
// - 값 판정: mipGenSettings Sharpen·Blur·Angular·LeaveExistingMips, encodingOverride가 None·Linear·
//   sRGB 밖, colorSpace가 None·sRGB 밖, mipLoadOptions OnlyFirstMip은 UnsupportedSetting,
//   compressionSettings LQ·EncodedReflectionCapture는 InvalidSetting이다.
// - 레코드 없는 타입(손 함수표 파생)은 값 판정만 한다.
//
// 오류(out은 빈다): 에디터 데이터가 없는 빌드(ROCKS_WITH_EDITOR_DATA 0) Unsupported, Texture가 아닌
// 객체 TypeMismatch, 객체 접근·FindChangedFields의 오류(레코드 실패 포함), 할당 실패 OutOfMemory.
//
// 사용 예(빌드 앞):
//   Array<TextureSettingIssue> issues(allocator);
//   auto checked = CheckTextureSettings(registry, access, texture, allocator, issues);
//   if (!checked) { return checked; }
//   if (!issues.IsEmpty()) { /* 진단으로 내고 빌드하지 않는다 */ }
ROCKS_TEXTURE_API Result<void> CheckTextureSettings(ObjectRegistry& registry,
                                                    const ObjectAccessContext& access,
                                                    ObjectHandle texture, Allocator& scratch,
                                                    Array<TextureSettingIssue>& out) noexcept;

} // namespace rocks
