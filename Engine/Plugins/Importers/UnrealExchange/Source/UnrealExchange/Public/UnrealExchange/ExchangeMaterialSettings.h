#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Material/MaterialPreviewSettings.h>
#include <MaterialSchema/MaterialTraitsDesc.h>
#include <MaterialSchema/VertexInputUsage.h>
#include <Object/ObjectRegistry.h>
#include <UnrealExchange/ExchangeBundle.h>
#include <UnrealExchange/ExchangeSettings.h>

// 머티리얼 설정 번역이다(Docs/Design/UnrealExchange 8.7절 7번 "구현에서 정함" — UX4b1a). UMaterial
// 덤프(settings)의 최상위 키마다 행 하나인 표로 Material.traits(MaterialTraitsDesc)를 만들고, 사용
// 플래그(bUsedWith*)는 Material.usage, 미리보기 카메라(EditorPitch·EditorYaw)는 Material.preview에
// 옮긴다. 표는 MaterialSystem 19.9–19.11절 대응을 따른다: "필드"는 옮기고, "필드 + TODO"는
// Unsupported(값이 UE 기본값과 다르면 경고 뒤 버림), "다른 곳"·"에디터 데이터"·"제외"는 Ignore다.
// traits의 하위 struct 칸(translucency·forward·postProcess)은 행 묶음으로 쓴다. 경고 detail은 이
// 표의 행 번호다(모르는 키는 ExchangeUnknownSettingDetail).
//
// 사용 예(UX4b1b 머티리얼 임포터 안):
//   ExchangeMaterialSettingsInput input;
//   input.asset = &asset;
//   input.substrate = bundle.substrate;
//   input.registry = &context.Registry();
//   input.access = context.Access();
//   Array<ExchangeSettingWarning> warnings(allocator);
//   ExchangeMaterialFailure failure;
//   if (auto translated = TranslateExchangeMaterialSettings(input, material.traits, material.usage,
//                                                           material.preview, warnings, failure);
//       !translated) {
//       return context.Fail(failure.code, translated.Error(), failure.detail);
//   }
namespace rocks {

// 표의 행 수다(옮김 78, 버림 28, Rocks 필드 없음 43 — UE 5.8 UMaterial 덤프 키 전부).
inline constexpr usize ExchangeMaterialSettingRowCount = 149;

// 표에서 변환이 직접 읽거나 경고에 쓰는 행의 번호다.
enum class ExchangeMaterialSettingRow : uint32 {
    MaterialDomain = 4,
    BlendMode = 5,
    UsedWithStaticMesh = 72,
    EditorPitch = 95,
    EditorYaw = 96
};

// 행 묶음 번호다(ExchangeSettingRow::group — 0은 traits 최상위).
enum class ExchangeMaterialSettingGroup : uint8 {
    Traits = 0,
    Translucency = 1,
    Forward = 2,
    PostProcess = 3
};

// 머티리얼 표다(정적 수명).
ROCKS_UNREALEXCHANGE_API Span<const ExchangeSettingRow> ExchangeMaterialSettingRows() noexcept;

// 행 묶음의 출력 키다(translucency·forward·postProcess — 묶음 n의 키는 n - 1 번째, 정적 수명).
ROCKS_UNREALEXCHANGE_API Span<const StringView> ExchangeMaterialSettingGroups() noexcept;

struct ExchangeMaterialSettingsInput {
    // 머티리얼 교환 에셋(Ready — 문서를 읽은 것)이다.
    const ExchangeAsset* asset = nullptr;
    // 묶음의 projectSettings.substrate다(ColoredTransmittance 경고 — 8.8절 3번).
    bool substrate = false;
    // ImportJson의 문맥(타입 표·Name pool — 출력이 쓸 Registry)이다.
    ObjectRegistry* registry = nullptr;
    ObjectAccessContext access{};
};

// 실패의 진단 코드와 detail이다. 할당·이름 pool 오류는 None이다.
struct ExchangeMaterialFailure {
    AssetDiagnosticCode code = AssetDiagnosticCode::None;
    uint64 detail = 0;
};

// input.asset의 settings를 번역해 traits(기본값에서 다시 만든다)·usage(덤프로 다시 정한다)·
// preview(기본값에서 카메라만 채운다)에 쓰고 경고를 warnings 뒤에 더한다(설정 순서 — 묶음 행의
// 경고는 최상위 경고 뒤, 끝에 ColoredTransmittance 경고). 사용 대상은 bUsedWithStaticMesh 참이면
// (키가 없으면 UE 기본값 true) StaticMesh 하나다(skinned 정점 입력 이름은 RenderBase RB5 전이라
// 옮기지 않는다 — 쿠킹 유도가 모은다).
//
// 경고: 설정 번역의 UnsupportedSetting(detail = 행 번호 — 필드 + TODO의 기본값 아닌 값, Rocks에
// 없는 enum 값(MD_RuntimeVirtualTexture·BLEND_AlphaHoldout), 표에 없는 키), BlendMode가
// BLEND_TranslucentColoredTransmittance이고 input.substrate가 참이면 UnsupportedSetting(detail =
// BlendMode 행 — 값은 Translucent로 옮긴다).
//
// 오류(failure.code·detail): 호출 계약(asset·registry·Name pool 없음, 머티리얼이 아님)
// InvalidSource / InvalidArgument, 번역의 값 종류 위반 InvalidSource / CorruptData(detail 0),
// ImportJson 진단 거부 InvalidSource / CorruptData(detail 0), 사용 플래그·미리보기 값의 종류 위반
// CorruptBundle / CorruptData(detail = 그 행), 행 표 결함·진단 없는 ImportJson 실패·타입 표·이름
// pool·writer·할당 오류는 그 오류(코드 None). 실패하면 출력 내용은 정해지지 않는다.
ROCKS_UNREALEXCHANGE_API Result<void> TranslateExchangeMaterialSettings(
    const ExchangeMaterialSettingsInput& input, material::MaterialTraitsDesc& traits,
    material::VertexInputUsage& usage, MaterialPreviewSettings& preview,
    Array<ExchangeSettingWarning>& warnings, ExchangeMaterialFailure& failure) noexcept;

} // namespace rocks
