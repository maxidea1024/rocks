#pragma once

#include <UnrealExchange/ExchangeSettings.h>

// 교환 메시 계열 설정 번역 표가 같이 쓰는 행 생성 함수와 멤버 표다(Docs/Design/UnrealExchange
// 5.5절 4번·6.4절 2번). UnrealExchange 모듈 안에서만 쓴다.
namespace rocks::exchange_detail {

using SettingKind = ExchangeSettingKind;

// 행을 만든다(모든 칸을 채운다).
inline constexpr ExchangeSettingRow Row(StringView key,
                                        SettingKind kind = SettingKind::Same) noexcept {
    return ExchangeSettingRow{key, kind, 0, nullptr, 0, StringView(), StringView()};
}

// 출력 키가 UE 키와 다른 행이다(5.5절 4번).
inline constexpr ExchangeSettingRow Renamed(StringView key, StringView rocksKey) noexcept {
    return ExchangeSettingRow{key, SettingKind::Same, 0, nullptr, 0, StringView(), rocksKey};
}

template <usize Count>
constexpr ExchangeSettingRow StructRow(StringView key, SettingKind kind,
                                       const ExchangeSettingRow (&members)[Count]) noexcept {
    return ExchangeSettingRow{key, kind, 0, members, Count, StringView(), StringView()};
}

inline constexpr ExchangeSettingRow UnsupportedRow(StringView key,
                                                   StringView defaultValue) noexcept {
    return ExchangeSettingRow{key,         SettingKind::Unsupported, 0, nullptr, 0, defaultValue,
                              StringView()};
}

// 값 이름 대응 하나(EnumMap 멤버 — UE 값 이름 → Rocks 열거자 이름)다.
inline constexpr ExchangeSettingRow EnumValue(StringView ueValue, StringView rocksValue) noexcept {
    return ExchangeSettingRow{ueValue, SettingKind::Same, 0, nullptr, 0, StringView(), rocksValue};
}

// UE FMeshUVChannelInfo → MeshUvDensity.
inline constexpr ExchangeSettingRow UvDensityMembers[] = {
    Row("bInitialized"),
    Row("bOverrideDensities"),
    Row("LocalUVDensities"),
};

// UE FStaticMaterial(5.5절 6번 — 스킨 메시 FSkeletalMaterial도 같은 모양, 6.4절 2번).
inline constexpr ExchangeSettingRow SlotMembers[] = {
    Row("MaterialInterface", SettingKind::Reference),
    Row("MaterialSlotName"),
    Row("ImportedMaterialSlotName"),
    StructRow("UVChannelData", SettingKind::Struct, UvDensityMembers),
    Row("OverlayMaterialInterface", SettingKind::Reference),
};

// UE FMeshNaniteSettings → VirtualGeometrySettings(이름은 대소문자만 다르다).
inline constexpr ExchangeSettingRow NaniteMembers[] = {
    Row("bEnabled"),
    Row("bExplicitTangents"),
    Row("bLerpUVs"),
    Row("bSeparable"),
    Row("bVoxelNDF"),
    Row("bVoxelOpacity"),
    Row("ShapePreservation", SettingKind::Enum),
    Row("PositionPrecision"),
    Row("NormalPrecision"),
    Row("TangentPrecision"),
    Row("BoneWeightPrecision"),
    Row("TargetMinimumResidencyInKB"),
    Row("KeepPercentTriangles"),
    Row("TrimRelativeError"),
    Row("GenerateFallback", SettingKind::Enum),
    Row("FallbackTarget", SettingKind::Enum),
    Row("FallbackPercentTriangles"),
    Row("FallbackRelativeError"),
    Row("MaxEdgeLengthFactor"),
    Row("NumRays"),
    Row("VoxelLevel"),
    Row("RayBackUp"),
    Row("DisplacementUVChannel"),
    UnsupportedRow("DisplacementMaps", "[]"),
    UnsupportedRow("NaniteAssemblyData", R"({"Parts": [], "Nodes": []})"),
};

// UE USkeletalMeshSocket 프로퍼티(스켈레톤·스킨 메시 소켓 — 6.4절 1·2번).
inline constexpr ExchangeSettingRow BoneSocketPropertyMembers[] = {
    Row("SocketName"),       Row("BoneName"),      Row("RelativeLocation"),
    Row("RelativeRotation"), Row("RelativeScale"), Row("bForceAlwaysAnimated"),
};

// 하위 객체 덤프 { subobject, class, properties }(11.7절 13번)의 소켓이다.
inline constexpr ExchangeSettingRow BoneSocketMembers[] = {
    Row("subobject"),
    Row("class", SettingKind::Ignore),
    StructRow("properties", SettingKind::Struct, BoneSocketPropertyMembers),
};

} // namespace rocks::exchange_detail
