#include "CurveImportSupport.h"
#include "CurveTableBuild.h"

#include <Core/Text/Json.h>
#include <Core/Text/NumberText.h>
#include <CurveAssetTools/CurveTableJsonImporter.h>
#include <CurveAssets/CurveTableAsset.generated.h>
#include <CurveAssets/CurveTableImportOptions.generated.h>
#include <Object/ObjectRegistry.h>
#include <Platform/PlatformMath.h>

// JSON 커브 표 임포터다(Docs/Design/CurveAssets 5.1.3절, UE UCurveTable::CreateTableFromJSONString
// — CurveTable.cpp:847-1003).
namespace rocks {

namespace {

constexpr Guid CurveTableJsonImporterId{0x2f8d6b14a07e4c39ull, 0x96c5d1e2b7a04f83ull};

// 행 이름 키다(UE RowNameJsonKey).
constexpr StringView RowNameKey = "Name";

// key가 행 이름 키인지다(UE FJsonObject는 키를 대소문자 무시로 비교한다).
bool IsRowNameKey(StringView key) noexcept {
    return key.ByteCount() == RowNameKey.ByteCount() && key.StartsWith(RowNameKey);
}

// JSON 값을 숫자로 읽는다(UE FJsonValue::TryGetNumber — 숫자는 double을 거쳐 float, 문자열은
// IsNumeric 규칙, bool은 1·0). 비유한은 실패다.
Result<float> JsonNumber(const JsonValue& value) noexcept {
    if (value.IsNumber()) {
        auto parsed = value.TryGetDouble();
        if (!parsed) {
            return Result<float>::Failure(ErrorCode::InvalidArgument);
        }

        const float number = static_cast<float>(parsed.Value());
        if (!platform::PlatformMath::IsFinite(number)) {
            return Result<float>::Failure(ErrorCode::InvalidArgument);
        }

        return Result<float>::Success(number);
    }

    if (value.IsString()) {
        return curve_import_detail::ParseJsonNumberString(value.GetString());
    }

    if (value.IsBool()) {
        return Result<float>::Success(value.GetBool() ? 1.0f : 0.0f);
    }

    return Result<float>::Failure(ErrorCode::InvalidArgument);
}

// 행 이름 텍스트다(UE GetStringField → AsString: 문자열 그대로, bool은 "true"·"false", 숫자는 원문
// 토큰 — UE SanitizeFloat 서식과 다를 수 있다(설계 7.2절 18), 그 밖은 빈 이름).
StringView RowNameText(const JsonValue& value) noexcept {
    if (value.IsString()) {
        return value.GetString();
    }

    if (value.IsBool()) {
        return value.GetBool() ? StringView("true") : StringView("false");
    }

    return value.IsNumber() ? value.RawNumber() : StringView();
}

} // namespace

CurveTableJsonImporter::CurveTableJsonImporter() noexcept {
    info_.id = CurveTableJsonImporterId;
    info_.name = CurveTableJsonImporterName;
    info_.version = CurveTableJsonImporterVersion;
    info_.outputType = TypeIdOf<CurveTableAsset>();
    info_.optionsType = TypeIdOf<CurveTableImportOptions>();
}

Result<void> CurveTableJsonImporter::Import(AssetImportContext& context) noexcept {
    using namespace curve_import_detail;
    CurveTableImportOptions options;
    if (const void* given = context.Options(); given != nullptr) {
        options = *static_cast<const CurveTableImportOptions*>(given);
    }

    if (!IsKnownInterp(options.interp)) {
        return context.Fail(AssetDiagnosticCode::InvalidImportOption, ErrorCode::InvalidArgument,
                            0);
    }

    ObjectRegistry& registry = context.Registry();
    NamePool* names = registry.Names();
    if (names == nullptr) {
        return context.Fail(AssetDiagnosticCode::None, ErrorCode::InvalidArgument, 0);
    }

    Allocator& allocator = registry.ObjectAllocator();
    String text(allocator);
    if (auto read = ReadSourceText(context, text); !read) {
        return read;
    }

    JsonDocument document(allocator);
    const JsonError error = document.Parse(text.View());
    if (!error.Succeeded()) {
        // 문서 오류의 detail은 0이다(원소 번호가 아니다 — 줄 위치는 원소가 정해지지 않는다).
        return error.code == JsonErrorCode::OutOfMemory
                   ? Result<void>::Failure(ErrorCode::OutOfMemory)
                   : FailFormat(context, 0);
    }

    const JsonValue root = document.Root();
    if (!root.IsArray() || root.Size() == 0) {
        return FailFormat(context, 0);
    }

    CurveTableAsset staged(allocator);
    BeginTable(staged, options.interp);
    Array<float> times(allocator);
    Array<float> values(allocator);
    auto warn = [&context](uint64 position) noexcept {
        return context.Warn(AssetDiagnosticCode::CurveImportRowSkipped, position);
    };

    for (usize index = 0; index < root.Size(); ++index) {
        const uint64 position = index + 1;
        const JsonValue element = root.At(index);
        if (!element.IsObject()) {
            if (auto warned = warn(position); !warned) {
                return warned;
            }

            continue;
        }

        // 행 이름: 대소문자를 무시해 찾고, 여럿이면 뒤의 것이다(UE TMap Add가 덮어쓴다).
        JsonValue nameValue;
        for (usize member = 0; member < element.Size(); ++member) {
            if (IsRowNameKey(element.KeyAt(member))) {
                nameValue = element.ValueAt(member);
            }
        }

        auto name = MakeRowName(*names, nameValue.IsValid() ? RowNameText(nameValue) : StringView(),
                                allocator);
        if (!name) {
            return FailRowName(context, name.Error(), position);
        }

        if (name.Value().IsNone() || staged.FindRow(name.Value()) != nullptr) {
            if (auto warned = warn(position); !warned) {
                return warned;
            }

            continue;
        }

        times.Clear();
        values.Clear();
        for (usize member = 0; member < element.Size(); ++member) {
            if (IsRowNameKey(element.KeyAt(member))) {
                continue;
            }

            // 시간 키·값이 읽히지 않으면 그 항목만 건너뛴다(UE와 같다 — 행은 남는다).
            auto time = ParseNumberCell(element.KeyAt(member));
            auto value = JsonNumber(element.ValueAt(member));
            if (!time || !value) {
                if (auto warned = warn(position); !warned) {
                    return warned;
                }

                continue;
            }

            if (auto pushed = times.TryPushBack(time.Value()); !pushed) {
                return pushed;
            }

            if (auto pushed = values.TryPushBack(value.Value()); !pushed) {
                return pushed;
            }
        }

        // 한 행 안의 같은 시간은 그 행 전체를 건너뛴다(UE FindDuplicateXValues → continue).
        bool duplicate = false;
        for (usize first = 0; first < times.Size() && !duplicate; ++first) {
            for (usize second = first + 1; second < times.Size(); ++second) {
                duplicate = duplicate || times[first] == times[second];
            }
        }

        if (duplicate) {
            if (auto warned = warn(position); !warned) {
                return warned;
            }

            continue;
        }

        if (auto added =
                AddTableRow(staged, name.Value(), times.View(), values.View(), options.interp);
            !added) {
            return added;
        }
    }

    return PublishTable(context, info_.outputType, staged);
}

} // namespace rocks
