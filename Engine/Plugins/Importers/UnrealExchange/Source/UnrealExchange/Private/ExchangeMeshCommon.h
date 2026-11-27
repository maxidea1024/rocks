#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Math/Quaternion.h>
#include <Core/Math/Rotator.h>
#include <Core/Math/Vector3.h>
#include <Core/Text/Json.h>
#include <Core/Text/NamePool.h>
#include <Core/Text/String.h>
#include <AnimationTools/SkeletonImport.h>
#include <MeshTools/StaticMeshImport.h>
#include <Object/ObjectRegistry.h>
#include <ReflectionText/TextJson.h>
#include <UnrealExchange/ExchangeMeshSource.h>
#include <UnrealExchange/ExchangeSettings.h>

#include "ExchangeJsonRead.h"

// 교환 메시 계열 변환(정적 메시·스켈레톤·스킨 메시 — Docs/Design/UnrealExchange 5.5절·6.4절 7번)이
// 같이 쓰는 값 읽기와 실패·경고 문맥이다. UnrealExchange 모듈 안에서만 쓴다.
namespace rocks::exchange_detail {

// UE FName None의 덤프 이름이다(빈 이름으로 본다).
inline constexpr StringView NoneName = "None";

template <usize Count>
usize FindName(const StringView (&names)[Count], StringView name) noexcept {
    for (usize index = 0; index < Count; ++index) {
        if (names[index] == name) {
            return index;
        }
    }

    return Count;
}

// UE None을 빈 이름으로 본다.
inline StringView WithoutNone(StringView name) noexcept {
    return name == NoneName ? StringView() : name;
}

// 기록 안 blob 참조 객체({blob, bytes, layout, components, count} — 3.3절)가 가리키는
// asset.blobs 번호다(digest·배치·components·count가 같은 항목). 없으면 asset.blobs 수다.
usize FindAssetBlob(const ExchangeAsset& asset, JsonValue reference) noexcept;

// 교환 실수(3.2절)다.
bool Number(JsonValue value, double& out) noexcept;

// 없는 키는 그대로 두고 참, 있으면 종류가 맞아야 참이다(아래 Opt* 모두).
bool OptNumber(JsonValue value, double& out) noexcept;
bool OptFloat(JsonValue value, float& out) noexcept;
bool OptBool(JsonValue value, bool& out) noexcept;
bool OptInt32(JsonValue value, int32& out) noexcept;
bool OptString(JsonValue value, StringView& out) noexcept;
// UE FVector `{X, Y, Z}`다.
bool OptVector(JsonValue value, Vec3d& out) noexcept;
// UE FRotator `{Pitch, Yaw, Roll}`(도 — Rocks Rotator와 같은 순서·단위)다.
bool OptRotator(JsonValue value, Rotator<double>& out) noexcept;
// UE FQuat `{X, Y, Z, W}`다.
bool OptQuat(JsonValue value, Quatd& out) noexcept;
// 없는 키는 빈 배열로 보고, 있으면 배열이어야 한다.
bool OptArray(JsonValue value) noexcept;
// PerPlatform 값(번역 결과 `{defaultValue}`)의 정수 기본값이다.
bool PerPlatformInt(JsonValue value, int32& out) noexcept;

// 변환 하나의 실패·경고·ImportJson 문맥이다(모두 빌림 — 변환 동안 산다).
struct ExchangeMeshContext {
    ExchangeMeshFailure& failure;
    Allocator& allocator;
    ObjectRegistry& registry;
    ObjectAccessContext access;
    // 참조로 걸 대상이다(null이면 참조는 모두 비운다).
    const OrderedMap<String, AssetId>* references;
    NamePool& names;
    Array<ExchangeSettingWarning>& warnings;

    // 할당 실패는 코드 없이, 그 밖은 code로 실패한다.
    Result<void> Fail(AssetDiagnosticCode code, ErrorCode error, uint64 detail) noexcept;

    Result<void> Corrupt(uint64 detail) noexcept {
        return Fail(AssetDiagnosticCode::CorruptBundle, ErrorCode::CorruptData, detail);
    }

    Result<void> Warn(AssetDiagnosticCode code, uint64 detail, StringView key) noexcept;

    // 번역한 JSON 객체 json을 값 struct value에 ImportJson한다(없는 키면 그대로). 진단을 남긴
    // 거부는 덤프 내용의 문제다(InvalidSource / CorruptData — 10.3절 오류 값). 진단이 없는
    // 실패(타입 표·문맥·한도·할당)는 코드 없이 그대로다.
    template <class T>
    Result<void> ImportValue(JsonValue json, T& value, uint64 detail) noexcept {
        if (!json.IsValid()) {
            return Result<void>::Success();
        }

        auto lease = registry.Types().AcquireValueType(TypeIdOf<T>(), TypeKind::Struct);
        if (!lease) {
            return Result<void>::Failure(lease.Error());
        }

        reflection_text::TextCodecOptions codec;
        reflection_text::TextDiagnostics diagnostics(allocator);
        auto prepared =
            reflection_text::ImportJson(json,
                                        reflection_text::AssignmentTarget::Value(
                                            registry, access, lease.Value(), &value, allocator),
                                        codec, diagnostics);
        if (!prepared) {
            const bool content =
                prepared.Error() == ErrorCode::InvalidArgument && diagnostics.Count() != 0;
            return content
                       ? Fail(AssetDiagnosticCode::InvalidSource, ErrorCode::CorruptData, detail)
                       : Result<void>::Failure(prepared.Error());
        }

        auto published = prepared.Value().Publish();
        return published ? Result<void>::Success() : Result<void>::Failure(published.Error());
    }

    // 번역한 참조(objectPath 문자열 — references에 있는 것만 남았다 — 또는 null)의 id다.
    Result<void> TranslatedReference(JsonValue value, AssetId& out, uint64 detail) noexcept;

    // 덤프 원문의 참조(`{objectPath}` 또는 null)의 id다(references에 없으면 비운다).
    Result<void> DumpReference(JsonValue value, AssetId& out, bool& present,
                               uint64 detail) noexcept;

    // object를 rows로 번역해 out에 읽어 둔다(경고는 warnings 뒤). 번역의 CorruptData만 덤프
    // 내용(InvalidSource / CorruptData, detail 0)이고 행 표 결함·writer·다시 읽기 실패는 코드 없이
    // 그 오류다.
    Result<void> Translate(JsonValue object, Span<const ExchangeSettingRow> rows,
                           ExchangeClass output, JsonDocument& out) noexcept;

    // 번역한 슬롯 배열(UE FStaticMaterial·FSkeletalMaterial — 5.5절 6번)을 out 뒤에 더한다(이름은
    // 번역 문서를 가리킨다).
    Result<void> ReadSlots(JsonValue items, Array<StaticMeshImportSlot>& out,
                           uint64 detail) noexcept;

    // data.bones(6.3절 1번)를 out에 둔다. retargetModes가 null이 아니면 본마다 retargetMode도
    // 읽는다(스켈레톤). 형식 위반은 CorruptBundle(detail = 본 번호)이다.
    Result<void> ReadBones(JsonValue bones, Array<SkeletonImportBone>& out,
                           Array<BoneTranslationRetarget>* retargetModes) noexcept;

    // 번역한 소켓 배열(USkeletalMeshSocket 하위 객체 덤프 — 6.4절 1번)을 out에 둔다. 공유 항목
    // ({subobject}만)·형식 위반은 CorruptBundle(detail)이다.
    Result<void> ReadBoneSockets(JsonValue items, Array<SkeletonImportSocket>& out,
                                 uint64 detail) noexcept;
};

} // namespace rocks::exchange_detail
