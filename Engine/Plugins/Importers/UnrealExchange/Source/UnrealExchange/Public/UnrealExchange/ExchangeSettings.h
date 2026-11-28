#pragma once

#include <Base/NumericLimits.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Containers/Span.h>
#include <Core/Text/Json.h>
#include <Core/Text/String.h>
#include <Object/AssetId.h>
#include <UnrealExchange/ExchangeBundle.h>

// UE 설정 덤프 번역이다(Docs/Design/UnrealExchange 10.6절 8번): 에셋 JSON의 settings(UE 프로퍼티
// 이름 그대로 — 11.5절)를 클래스마다의 행 표로 Rocks 리플렉션 JSON(ReflectionText ImportJson 표기)
// 객체 하나로 바꾼다. 키 짝맞추기는 ImportJson의 대소문자 무시를 그대로 쓴다(UE `LODGroup` → Rocks
// `lodGroup`). 표에 없는 키·Rocks 필드가 없는 UE 설정의 기본값 아닌 값은 경고로 알리고 버린다.
//
// 사용 예(A2f 임포터 — 텍스처 표는 ExchangeTextureSettings.h):
//   ExchangeSettingInput input;
//   input.settings = asset.settings;
//   input.rows = ExchangeTextureSettingRows();
//   input.output = asset.exchangeClass;
//   input.references = &options.references;
//   StringBuilder<> text(allocator);
//   JsonWriter writer(text);
//   Array<ExchangeSettingWarning> warnings(allocator);
//   if (auto translated = TranslateExchangeSettings(input, writer, warnings);
//       !translated || !writer.Finish()) {
//       ...
//   }
namespace rocks {

// 행 하나의 처리 종류다(10.6절 8번).
enum class ExchangeSettingKind : uint8 {
    // 키의 bool 접두 b(뒤가 대문자)만 떼고 값을 그대로 옮긴다.
    Same,
    // UE enum 값 이름의 접두(첫 `_`까지, 대문자·숫자만)를 뗀다. 접두가 없거나 정수 값이면 그대로다.
    Enum,
    // UE FGuid `{A,B,C,D}`(int32 넷)를 DigitsWithHyphens 순서의 36자 소문자 문자열로 바꾼다.
    Guid,
    // UE FPerPlatformFloat `{Default, PerPlatform}` → `{defaultValue}`(PerPlatform이 비지 않으면
    // UnsupportedSetting 경고).
    PerPlatformFloat,
    // 객체 참조 `{objectPath}`·null → resolver 텍스트(objectPath) 또는 null(references에 없음).
    Reference,
    // 객체 참조 배열 → resolver 텍스트 배열(references에 없는 원소·null 원소는 뺀다).
    ReferenceArray,
    // 값 struct — members 표로 재귀한다.
    Struct,
    // 값 struct 배열 — 원소마다 members 표로 재귀한다.
    StructArray,
    // 값 이름이 Rocks와 다른 enum — members 표의 ueKey(UE 값 이름) → rocksKey(Rocks 열거자 이름)로
    // 바꾼다(UnrealExchange 6.4절 3번). 표에 없는 값·문자열이 아닌 값은 CorruptData다. rocksKey가
    // 빈 멤버는 Rocks에 없는 값이다: UnsupportedSetting 경고 뒤 키를 버린다(Rocks 기본값이 남는다 —
    // UX4b1a).
    EnumMap,
    // 값 struct의 멤버를 members 표로 형제 키에 쓴다(출력 키는 멤버 행의 rocksKey — UE FIntPoint
    // `{X, Y}` → userTextureDivisorX·Y, UX4b1a). 멤버 행은 Same·Enum·EnumMap만이다. 멤버 표에 없는
    // 멤버와 EnumMap 멤버의 Rocks에 없는 값은 UnsupportedSetting(detail = 그 행 번호)이다.
    Flatten,
    // 설정이 아닌 UE 멤버 — 버린다.
    Ignore,
    // Rocks 필드가 없는 UE 설정 — 값이 defaultValue와 다르면 UnsupportedSetting 경고 뒤 버린다.
    Unsupported
};

// 표에 없는 키의 UnsupportedSetting detail이다(행 번호가 아니다).
inline constexpr uint64 ExchangeUnknownSettingDetail = NumericLimits<uint32>::Max();

// 교환 클래스 집합의 비트다(ExchangeSettingRow::classes).
constexpr uint32 ExchangeClassBit(ExchangeClass type) noexcept {
    return 1u << static_cast<uint32>(type);
}

// 번역 표의 행 하나다(constexpr 표 — 정적 수명).
struct ExchangeSettingRow {
    StringView ueKey;
    ExchangeSettingKind kind = ExchangeSettingKind::Same;
    // 이 행을 쓰는 출력 클래스 집합이다(ExchangeClassBit의 합 — 0이면 모든 클래스). 집합 밖
    // 클래스의 출력이면 경고 없이 버린다(다른 차원 클래스의 주소 키 등).
    uint32 classes = 0;
    // Struct·StructArray의 멤버 표, EnumMap의 값 대응 표다.
    const ExchangeSettingRow* members = nullptr;
    usize memberCount = 0;
    // Unsupported의 UE 기본값(JSON 텍스트)이다.
    StringView defaultValue;
    // 출력 키다(UnrealExchange 5.5절 4번). 비면 UE 키에서 bool 접두 b만 뗀 이름이다(대소문자는
    // ImportJson이 무시한다 — 이름이 다른 행만 채운다, 예: BuildScale3D → buildScale).
    StringView rocksKey;
    // 최상위 표의 행 묶음이다(0 = 최상위 객체, n = ExchangeSettingInput::groups[n - 1] 키의 하위
    // 객체 — Rocks 필드가 하위 struct에 있을 때, 10.6절 8번). 멤버 표에서는 0이어야 한다.
    uint8 group = 0;
};

// 경고 하나다. key는 settings 문서를 가리킨다(문서가 살아 있는 동안 유효).
struct ExchangeSettingWarning {
    AssetDiagnosticCode code = AssetDiagnosticCode::UnsupportedSetting;
    // 최상위 표의 행 번호(struct 멤버의 경고는 그 struct 행 번호), 표에 없는 최상위 키는
    // ExchangeUnknownSettingDetail이다.
    uint64 detail = 0;
    StringView key;
};

struct ExchangeSettingInput {
    // 에셋 JSON의 settings 객체다.
    JsonValue settings;
    Span<const ExchangeSettingRow> rows;
    // 출력의 교환 클래스다(행 classes와 대조).
    ExchangeClass output = ExchangeClass::Unsupported;
    // 참조로 걸 대상이다(objectPath → AssetId — 키만 본다). null이면 참조는 모두 null이다.
    const OrderedMap<String, AssetId>* references = nullptr;
    // 행 묶음의 출력 키다(ExchangeSettingRow::group n의 키는 groups[n - 1]). 묶음 객체는 최상위 키
    // 뒤에 이 순서로, 비어 있어도 쓴다.
    Span<const StringView> groups{};
};

// input.settings를 번역해 writer에 JSON 객체 하나를 쓰고(Finish는 호출자) 경고를 warnings 뒤에
// 더한다(설정 순서 — 묶음 행의 경고는 최상위 경고 뒤 묶음 순서). 키 순서는 settings 순서이고 묶음
// 행은 최상위 키 뒤 묶음 객체 안에 쓴다.
//
// 오류: settings가 객체가 아님·행 종류와 맞지 않는 값(Guid가 int32 넷 객체가 아님, 참조가 null도
// objectPath 객체도 아님, 배열·객체 종류 불일치, PerPlatformFloat의 Default가 없음, Flatten 값이
// 객체가 아님 — 덤프 내용) CorruptData, 행 표의 결함(Unsupported 기본값 텍스트가 JSON이 아님,
// 최상위 행의 group이 groups 수보다 큼, 멤버 표 행의 group이 0이 아님, Flatten 멤버가 Same·Enum·
// EnumMap이 아니거나 group이 있음) InvalidArgument, writer 오류(중복 키·깊이 등 — writer는 실패
// 상태), 할당 실패. 실패하면 writer·warnings 결과를 버린다.
ROCKS_UNREALEXCHANGE_API Result<void>
TranslateExchangeSettings(const ExchangeSettingInput& input, JsonWriter& writer,
                          Array<ExchangeSettingWarning>& warnings) noexcept;

} // namespace rocks
