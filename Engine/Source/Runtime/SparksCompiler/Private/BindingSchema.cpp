#include <SparksCompiler/BindingSchema.h>

#include <Core/Algorithms.h>
#include <Core/Text/Json.h>
#include <Core/Text/StringBuilder.h>
#include <SparksCompiler/PackageManifest.h>
#include <SparksCompiler/Token.h>

#include "LanguageNames.h"
#include "Names.h"
#include "SchemaInput.h"

// binding schema reader다(Docs/Design/SparksLanguage.ko.md 10.2절 "binding schema 파일 형식"). 모든
// 형식 오류는 S1120이며 오류마다 하나를 내고 끝까지 읽는다.

namespace rocks::sparks {

namespace {

// 진단 문장 builder의 inline 크기다.
constexpr usize SchemaMessageInlineBytes = 192;

constexpr StringView SchemaFix = "Regenerate the binding schema; do not edit it by hand.";

constexpr StringView RootKeys[] = {"format", "package", "module", "types"};
constexpr StringView TypeKeys[] = {
    "name",          "id",     "kind",      "base",   "scriptExtensible",
    "constructible", "fields", "functions", "events", "cases"};
constexpr StringView FieldKeys[] = {"name", "id", "type", "access", "computed", "changed"};
constexpr StringView FunctionKeys[] = {
    "name",       "id",      "static",  "async", "overridable", "typeParameters",
    "parameters", "returns", "effects", "pure",  "oneShot"};
constexpr StringView FunctionParameterKeys[] = {"name", "type", "perSecond"};
constexpr StringView EventParameterKeys[] = {"name", "type"};
constexpr StringView TypeParameterKeys[] = {"name", "constraint"};
constexpr StringView EventKeys[] = {"name", "id", "parameters", "publisher"};

// 한 ID와 그 위치다(파일 안 중복 검사).
struct IdUse {
    SchemaText id;
    SourceSpan span;
};

// 한 타입 안 멤버 이름과 그 위치다(필드·함수·이벤트는 한 이름 공간).
struct NameUse {
    SchemaText name;
    SourceSpan span;
};

} // namespace

BindingSchema::BindingSchema(Allocator& allocator) noexcept
    : text_(allocator), types_(allocator), fields_(allocator), functions_(allocator),
      parameters_(allocator), typeParameters_(allocator), events_(allocator), cases_(allocator) {
}

bool IsStableIdText(StringView text) noexcept {
    if (text.ByteCount() != StableIdHexDigits) {
        return false;
    }

    bool nonZero = false;
    for (usize index = 0; index < text.ByteCount(); ++index) {
        const char value = text.Data()[index];
        if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'))) {
            return false;
        }

        nonZero = nonZero || value != '0';
    }

    return nonZero;
}

namespace detail {

class SchemaReader final {
public:
    SchemaReader(const SourceSet& sources, SourceId id, DiagnosticList& diagnostics,
                 BindingSchema& out) noexcept
        : file_(sources.Get(id)), id_(id), diagnostics_(diagnostics), out_(out),
          allocator_(out.text_.GetAllocator()), ids_(allocator_), typeNames_(allocator_),
          memberNames_(allocator_) {
    }

    Result<bool> Run() noexcept {
        out_.text_.Clear();
        out_.types_.Clear();
        out_.fields_.Clear();
        out_.functions_.Clear();
        out_.parameters_.Clear();
        out_.typeParameters_.Clear();
        out_.events_.Clear();
        out_.cases_.Clear();
        out_.package_ = SchemaText{};
        out_.module_ = SchemaText{};
        out_.moduleSpan_ = SourceSpan{};
        out_.source_ = id_;

        JsonDocument document(allocator_);
        const JsonError parsed = document.Parse(file_.Text());
        if (!parsed.Succeeded()) {
            if (parsed.code == JsonErrorCode::OutOfMemory) {
                return Result<bool>::Failure(ErrorCode::OutOfMemory);
            }

            const uint32 at = static_cast<uint32>(parsed.position.byteOffset);
            Report(SourceSpan{at, at}, "The binding schema is not valid JSON.",
                   JsonErrorMessage(parsed.code, JsonSyntax::Strict));
            return Finish();
        }

        const JsonValue root = document.Root();
        if (!root.IsObject()) {
            ReportAt(root, "The binding schema must be one JSON object.");
            return Finish();
        }

        constexpr StringView Required[] = {"format", "package", "module", "types"};
        CheckKeys(root, "binding schema", RootKeys, Required);
        const JsonValue format = root.Find("format");
        if (format.IsValid()) {
            const auto number = format.TryGetInt64();
            if (!number || number.Value() != static_cast<int64>(BindingSchemaFormat)) {
                ReportAt(format, "The binding schema format is not 1.");
            }
        }

        const JsonValue package = root.Find("package");
        if (package.IsValid()) {
            if (!package.IsString() || package.GetString() != EnginePackageName) {
                ReportAt(package, "The binding schema package must be 'Rocks'.");
            } else {
                Store(package.GetString(), out_.package_);
            }
        }

        const JsonValue module = root.Find("module");
        if (module.IsValid()) {
            if (!module.IsString() || !detail::IsPascalName(module.GetString())) {
                ReportAt(module, "The binding schema module must be a module name.");
            } else {
                out_.moduleSpan_ = SpanOf(module);
                Store(module.GetString(), out_.module_);
            }
        }

        const JsonValue types = root.Find("types");
        if (types.IsValid()) {
            if (!types.IsArray()) {
                ReportAt(types, "'types' must be an array.");
            } else {
                for (usize index = 0; index < types.Size() && error_ == ErrorCode::None; ++index) {
                    ReadType(types.At(index));
                }
            }
        }

        CheckDuplicateTypeNames();
        CheckDuplicateIds();
        return Finish();
    }

private:
    Result<bool> Finish() noexcept {
        if (error_ != ErrorCode::None) {
            return Result<bool>::Failure(error_);
        }

        return Result<bool>::Success(valid_);
    }

    SourceSpan SpanOf(JsonValue value) const noexcept {
        return detail::JsonValueSpan(file_.Text(), value.Position());
    }

    void Report(SourceSpan span, StringView message, StringView explanation) noexcept {
        valid_ = false;
        if (error_ != ErrorCode::None) {
            return;
        }

        if (auto added = diagnostics_.TryAdd(DiagnosticCode::InvalidBindingSchema, id_, span,
                                             message, explanation, SchemaFix);
            !added) {
            error_ = added.Error();
        }
    }

    void ReportAt(JsonValue value, StringView message) noexcept {
        Report(SpanOf(value), message, "");
    }

    // "<앞> '<이름>' <뒤>." 꼴의 문장을 만들어 낸다(이름은 key·schema 이름 — 인쇄 가능한 ASCII만
    // 넣고 그 밖은 생략한다).
    void ReportNamed(SourceSpan span, StringView before, StringView name,
                     StringView after) noexcept {
        constexpr char FirstPrintable = 0x20;
        constexpr char LastPrintable = 0x7E;
        bool quotable = !name.IsEmpty();
        for (usize index = 0; index < name.ByteCount(); ++index) {
            if (name.Data()[index] < FirstPrintable || name.Data()[index] > LastPrintable) {
                quotable = false;
            }
        }

        StringBuilder<SchemaMessageInlineBytes> message(allocator_);
        message.Append(before);
        if (quotable) {
            message.Append(" '").Append(name).Append("'");
        }

        message.Append(after);
        if (message.HasFailed()) {
            error_ = message.Status().Error();
            return;
        }

        Report(span, message.View().TrimAsciiStart(), "");
    }

    // 글자를 저장한다. 실패하면 out은 그대로다(저장소 없는 글자를 가리키지 않게).
    bool Store(StringView text, SchemaText& out) noexcept {
        const uint32 offset = static_cast<uint32>(out_.text_.Size());
        if (auto appended = out_.text_.TryAppend(Span<const char>(text.Data(), text.ByteCount()));
            !appended) {
            error_ = appended.Error();
            return false;
        }

        out.offset = offset;
        out.length = static_cast<uint32>(text.ByteCount());
        return true;
    }

    // object의 key가 allowed 안에 있는지, required가 모두 있는지 본다.
    void CheckKeys(JsonValue object, StringView what, Span<const StringView> allowed,
                   Span<const StringView> required) noexcept {
        for (usize index = 0; index < object.Size(); ++index) {
            const StringView key = object.KeyAt(index);
            bool known = false;
            for (const StringView candidate : allowed) {
                known = known || candidate == key;
            }

            if (!known) {
                StringBuilder<SchemaMessageInlineBytes> after(allocator_);
                after.Append(" is not a key of a ").Append(what).Append(".");
                if (after.HasFailed()) {
                    error_ = after.Status().Error();
                    return;
                }

                ReportNamed(SpanOf(object.ValueAt(index)), "Key", key, after.View());
            }
        }

        for (const StringView key : required) {
            if (!object.Find(key).IsValid()) {
                StringBuilder<SchemaMessageInlineBytes> after(allocator_);
                after.Append(" is missing from a ").Append(what).Append(".");
                if (after.HasFailed()) {
                    error_ = after.Status().Error();
                    return;
                }

                ReportNamed(SpanOf(object), "Key", key, after.View());
            }
        }
    }

    // 있으면 bool이어야 한다. 없으면 fallback.
    bool ReadBool(JsonValue object, StringView key, bool fallback) noexcept {
        const JsonValue value = object.Find(key);
        if (!value.IsValid()) {
            return fallback;
        }

        if (!value.IsBool()) {
            ReportNamed(SpanOf(value), "", key, " must be true or false.");
            return fallback;
        }

        return value.GetBool();
    }

    // 이름(규칙은 camel이면 lowerCamel, 아니면 PascalCase)을 읽어 저장한다. 없거나 틀리면 false.
    bool ReadName(JsonValue object, StringView key, bool camel, SchemaText& out) noexcept {
        const JsonValue value = object.Find(key);
        if (!value.IsValid()) {
            return false;
        }

        const bool valid = value.IsString() && (camel ? detail::IsCamelName(value.GetString())
                                                      : detail::IsPascalName(value.GetString()));
        if (!valid) {
            ReportNamed(SpanOf(value), "", key,
                        camel ? StringView(" must be a lowerCamel name.")
                              : StringView(" must be a PascalCase name."));
            return false;
        }

        // 키워드는 모두 소문자로 시작하므로 lowerCamel 이름만 겹칠 수 있다.
        if (IsKeywordText(value.GetString())) {
            ReportNamed(SpanOf(value), "", key, " must not be a keyword.");
            return false;
        }

        return Store(value.GetString(), out);
    }

    bool ReadId(JsonValue object, SchemaText& out, SourceSpan& span) noexcept {
        const JsonValue value = object.Find("id");
        if (!value.IsValid()) {
            return false;
        }

        if (!value.IsString() || !IsStableIdText(value.GetString())) {
            ReportAt(value, "'id' must be 32 lowercase hexadecimal digits, not all zero.");
            return false;
        }

        if (!Store(value.GetString(), out)) {
            return false;
        }

        IdUse use;
        use.id = out;
        use.span = SpanOf(value);
        span = use.span;
        if (auto pushed = ids_.TryPushBack(use); !pushed) {
            error_ = pushed.Error();
        }

        return true;
    }

    bool ReadTypeNotation(JsonValue object, StringView key, SchemaText& out) noexcept {
        const JsonValue value = object.Find(key);
        if (!value.IsValid()) {
            return false;
        }

        if (!value.IsString() || !detail::IsValidTypeNotation(value.GetString())) {
            ReportNamed(SpanOf(value), "", key,
                        " is not a type, as in 'Array<Actor>' or 'int32?'.");
            return false;
        }

        return Store(value.GetString(), out);
    }

    // 배열이면 그대로, 아니면 진단을 내고 무효 값을 돌려준다.
    JsonValue ArrayOf(JsonValue object, StringView key) noexcept {
        const JsonValue value = object.Find(key);
        if (value.IsValid() && !value.IsArray()) {
            ReportNamed(SpanOf(value), "", key, " must be an array.");
            return JsonValue();
        }

        return value;
    }

    // 한 타입 안 이름 공간(필드·함수·이벤트)에 이름을 더한다. 중복이면 진단.
    void UseMemberName(SchemaText name, SourceSpan span) noexcept {
        for (const NameUse& existing : memberNames_) {
            if (out_.Text(existing.name) == out_.Text(name)) {
                ReportNamed(span, "Member", out_.Text(name), " is declared twice in this type.");
                return;
            }
        }

        if (auto pushed = memberNames_.TryPushBack(NameUse{name, span}); !pushed) {
            error_ = pushed.Error();
        }
    }

    // kind에서 허용하지 않는 key를 진단한다.
    void RejectKey(JsonValue object, StringView key, StringView kinds) noexcept {
        const JsonValue value = object.Find(key);
        if (value.IsValid()) {
            StringBuilder<SchemaMessageInlineBytes> after(allocator_);
            after.Append(" is only for ").Append(kinds).Append(" types.");
            if (after.HasFailed()) {
                error_ = after.Status().Error();
                return;
            }

            ReportNamed(SpanOf(value), "", key, after.View());
        }
    }

    void ReadType(JsonValue object) noexcept {
        if (!object.IsObject()) {
            ReportAt(object, "Each type must be an object.");
            return;
        }

        constexpr StringView Required[] = {"name", "id", "kind"};
        CheckKeys(object, "type", TypeKeys, Required);
        SchemaType type;
        const JsonValue name = object.Find("name");
        if (ReadName(object, "name", false, type.name)) {
            type.span = SpanOf(name);
            if (detail::IsLanguageOwnedName(out_.Text(type.name))) {
                ReportNamed(type.span, "Type", out_.Text(type.name),
                            " uses a name that the language owns.");
            }

            // 중복은 다 읽은 뒤 정렬로 본다(CheckDuplicateTypeNames).
            if (auto pushed = typeNames_.TryPushBack(NameUse{type.name, type.span}); !pushed) {
                error_ = pushed.Error();
                return;
            }
        } else if (object.Find("name").IsValid()) {
            type.span = SpanOf(name);
        } else {
            type.span = SpanOf(object);
        }

        ReadId(object, type.id, type.idSpan);
        const JsonValue kind = object.Find("kind");
        bool knownKind = false;
        if (kind.IsValid()) {
            constexpr struct {
                StringView text;
                SchemaTypeKind kind;
            } Kinds[] = {{"object", SchemaTypeKind::Object},
                         {"asset", SchemaTypeKind::Asset},
                         {"struct", SchemaTypeKind::Struct},
                         {"enum", SchemaTypeKind::Enum},
                         {"interface", SchemaTypeKind::Interface}};
            for (const auto& candidate : Kinds) {
                if (kind.IsString() && kind.GetString() == candidate.text) {
                    type.kind = candidate.kind;
                    knownKind = true;
                }
            }

            if (!knownKind) {
                ReportAt(kind, "'kind' must be object, asset, struct, enum or interface.");
            }
        }

        // kind가 허용하지 않는 key는 진단 하나로 끝내고 값은 읽지 않는다. kind를 모르면(빠졌거나
        // 틀림) 모든 key를 읽는다.
        bool allowBase = true;
        bool allowExtensible = true;
        bool allowConstructible = true;
        bool allowFields = true;
        bool allowFunctions = true;
        bool allowEvents = true;
        bool allowCases = true;
        const bool isEnum = knownKind && type.kind == SchemaTypeKind::Enum;
        if (knownKind) {
            const SchemaTypeKind k = type.kind;
            const bool objectLike = k == SchemaTypeKind::Object || k == SchemaTypeKind::Asset;
            allowBase = objectLike || k == SchemaTypeKind::Interface;
            allowExtensible = objectLike;
            allowConstructible = k == SchemaTypeKind::Struct;
            allowFields = objectLike || k == SchemaTypeKind::Struct;
            allowFunctions = !isEnum;
            allowEvents = k == SchemaTypeKind::Object;
            allowCases = isEnum;
            if (!allowBase) {
                RejectKey(object, "base", "object, asset and interface");
            }

            if (!allowExtensible) {
                RejectKey(object, "scriptExtensible", "object and asset");
            }

            if (!allowConstructible) {
                RejectKey(object, "constructible", "struct");
            }

            if (!allowFields) {
                RejectKey(object, "fields", "object, asset and struct");
            }

            if (!allowFunctions) {
                RejectKey(object, "functions", "non-enum");
            }

            if (!allowEvents) {
                RejectKey(object, "events", "object");
            }

            if (!allowCases) {
                RejectKey(object, "cases", "enum");
            } else if (!object.Find("cases").IsValid()) {
                ReportAt(object, "An enum type needs 'cases'.");
            }
        }

        if (allowBase && object.Find("base").IsValid()) {
            type.baseSpan = SpanOf(object.Find("base"));
            ReadName(object, "base", false, type.base);
        }

        type.scriptExtensible = allowExtensible && ReadBool(object, "scriptExtensible", false);
        type.constructible = !allowConstructible || ReadBool(object, "constructible", true);

        memberNames_.Clear();
        type.firstField = static_cast<uint32>(out_.fields_.Size());
        const JsonValue fields = allowFields ? ArrayOf(object, "fields") : JsonValue();
        for (usize index = 0; index < fields.Size() && error_ == ErrorCode::None; ++index) {
            ReadField(fields.At(index));
        }

        type.fieldCount = static_cast<uint32>(out_.fields_.Size()) - type.firstField;
        type.firstFunction = static_cast<uint32>(out_.functions_.Size());
        const JsonValue functions = allowFunctions ? ArrayOf(object, "functions") : JsonValue();
        // kind를 모르면 overridable도 거부하지 않는다(kind 규칙과 같다).
        const bool overridableKind =
            !knownKind || type.kind == SchemaTypeKind::Object || type.kind == SchemaTypeKind::Asset;
        for (usize index = 0; index < functions.Size() && error_ == ErrorCode::None; ++index) {
            ReadFunction(functions.At(index), overridableKind);
        }

        type.functionCount = static_cast<uint32>(out_.functions_.Size()) - type.firstFunction;
        type.firstEvent = static_cast<uint32>(out_.events_.Size());
        const JsonValue events = allowEvents ? ArrayOf(object, "events") : JsonValue();
        for (usize index = 0; index < events.Size() && error_ == ErrorCode::None; ++index) {
            ReadEvent(events.At(index));
        }

        type.eventCount = static_cast<uint32>(out_.events_.Size()) - type.firstEvent;
        type.firstCase = static_cast<uint32>(out_.cases_.Size());
        if (allowCases) {
            ReadCases(object, isEnum);
        }

        type.caseCount = static_cast<uint32>(out_.cases_.Size()) - type.firstCase;
        if (auto pushed = out_.types_.TryPushBack(type); !pushed) {
            error_ = pushed.Error();
        }
    }

    // isEnum이면 case가 하나 이상이어야 한다(kind를 모르면 요소만 검사한다).
    void ReadCases(JsonValue object, bool isEnum) noexcept {
        const JsonValue cases = ArrayOf(object, "cases");
        if (!cases.IsValid()) {
            return;
        }

        if (cases.Size() == 0 && isEnum) {
            ReportAt(cases, "An enum type needs at least one case.");
            return;
        }

        const uint32 first = static_cast<uint32>(out_.cases_.Size());
        for (usize index = 0; index < cases.Size() && error_ == ErrorCode::None; ++index) {
            const JsonValue element = cases.At(index);
            if (!element.IsString() || !detail::IsPascalName(element.GetString())) {
                ReportAt(element, "Each enum case must be a PascalCase name.");
                continue;
            }

            bool duplicate = false;
            for (uint32 existing = first; existing < out_.cases_.Size() && !duplicate; ++existing) {
                duplicate = out_.Text(out_.cases_[existing].name) == element.GetString();
            }

            if (duplicate) {
                ReportNamed(SpanOf(element), "Enum case", element.GetString(), " is listed twice.");
                continue;
            }

            SchemaCase item;
            item.span = SpanOf(element);
            if (Store(element.GetString(), item.name)) {
                if (auto pushed = out_.cases_.TryPushBack(item); !pushed) {
                    error_ = pushed.Error();
                }
            }
        }
    }

    void ReadField(JsonValue object) noexcept {
        if (!object.IsObject()) {
            ReportAt(object, "Each field must be an object.");
            return;
        }

        SchemaField field;
        field.computed = ReadBool(object, "computed", false);
        constexpr StringView Required[] = {"name", "type"};
        CheckKeys(object, "field", FieldKeys, Required);
        const JsonValue id = object.Find("id");
        if (field.computed && id.IsValid()) {
            ReportAt(id, "A computed property has no 'id'.");
        } else if (!field.computed && !id.IsValid()) {
            ReportAt(object, "Key 'id' is missing from a field.");
        } else if (!field.computed) {
            ReadId(object, field.id, field.idSpan);
        }

        field.span = object.Find("name").IsValid() ? SpanOf(object.Find("name")) : SpanOf(object);
        if (ReadName(object, "name", false, field.name)) {
            UseMemberName(field.name, field.span);
        }

        if (object.Find("type").IsValid()) {
            field.typeSpan = SpanOf(object.Find("type"));
        }

        ReadTypeNotation(object, "type", field.type);
        const JsonValue access = object.Find("access");
        if (access.IsValid()) {
            if (access.IsString() && access.GetString() == "readWrite") {
                field.readWrite = true;
            } else if (!access.IsString() || access.GetString() != "read") {
                ReportAt(access, "'access' must be \"read\" or \"readWrite\".");
            }
        }

        field.changed = ReadBool(object, "changed", false);
        if (auto pushed = out_.fields_.TryPushBack(field); !pushed) {
            error_ = pushed.Error();
        }
    }

    // 매개변수 배열이다(함수는 perSecond까지, 이벤트는 이름·타입만).
    void ReadParameters(JsonValue object, bool function, uint32& first, uint32& count) noexcept {
        first = static_cast<uint32>(out_.parameters_.Size());
        const JsonValue parameters = ArrayOf(object, "parameters");
        for (usize index = 0; index < parameters.Size() && error_ == ErrorCode::None; ++index) {
            const JsonValue element = parameters.At(index);
            if (!element.IsObject()) {
                ReportAt(element, "Each parameter must be an object.");
                continue;
            }

            constexpr StringView Required[] = {"name", "type"};
            if (function) {
                CheckKeys(element, "parameter", FunctionParameterKeys, Required);
            } else {
                CheckKeys(element, "parameter", EventParameterKeys, Required);
            }

            SchemaParameter parameter;
            parameter.span =
                element.Find("name").IsValid() ? SpanOf(element.Find("name")) : SpanOf(element);
            if (ReadName(element, "name", true, parameter.name)) {
                for (uint32 existing = first; existing < out_.parameters_.Size(); ++existing) {
                    if (out_.Text(out_.parameters_[existing].name) == out_.Text(parameter.name)) {
                        ReportNamed(parameter.span, "Parameter", out_.Text(parameter.name),
                                    " is declared twice.");
                        break;
                    }
                }
            }

            if (element.Find("type").IsValid()) {
                parameter.typeSpan = SpanOf(element.Find("type"));
            }

            ReadTypeNotation(element, "type", parameter.type);
            parameter.perSecond = function && ReadBool(element, "perSecond", false);
            if (auto pushed = out_.parameters_.TryPushBack(parameter); !pushed) {
                error_ = pushed.Error();
            }
        }

        count = static_cast<uint32>(out_.parameters_.Size()) - first;
    }

    void ReadFunction(JsonValue object, bool overridableKind) noexcept {
        if (!object.IsObject()) {
            ReportAt(object, "Each function must be an object.");
            return;
        }

        constexpr StringView Required[] = {"name", "id"};
        CheckKeys(object, "function", FunctionKeys, Required);
        SchemaFunction function;
        function.span =
            object.Find("name").IsValid() ? SpanOf(object.Find("name")) : SpanOf(object);
        if (ReadName(object, "name", false, function.name)) {
            UseMemberName(function.name, function.span);
        }

        ReadId(object, function.id, function.idSpan);
        function.isStatic = ReadBool(object, "static", false);
        function.isAsync = ReadBool(object, "async", false);
        function.overridable = ReadBool(object, "overridable", false);
        if (function.overridable && !overridableKind) {
            ReportAt(object.Find("overridable"),
                     "'overridable' is only for functions of object and asset types.");
        } else if (function.overridable && function.isStatic) {
            ReportAt(object.Find("overridable"), "A static function cannot be overridable.");
        }

        function.pure = ReadBool(object, "pure", false);
        function.oneShot = ReadBool(object, "oneShot", false);
        const JsonValue effects = ArrayOf(object, "effects");
        for (usize index = 0; index < effects.Size() && error_ == ErrorCode::None; ++index) {
            const JsonValue element = effects.At(index);
            struct EffectName {
                StringView name;
                uint8 bit;
            };
            constexpr EffectName Names[] = {{"spawn", SchemaEffects::Spawn},
                                            {"acquire", SchemaEffects::Acquire},
                                            {"readiness", SchemaEffects::Readiness},
                                            {"io", SchemaEffects::Io},
                                            {"wallClock", SchemaEffects::WallClock}};
            uint8 bit = 0;
            for (const EffectName& name : Names) {
                if (element.IsString() && element.GetString() == name.name) {
                    bit = name.bit;
                }
            }

            if (bit == 0) {
                ReportAt(element, "An effect is one of 'spawn', 'acquire', 'readiness', 'io' and "
                                  "'wallClock'.");
            } else if ((function.effects & bit) != 0) {
                ReportAt(element, "An effect is listed twice.");
            } else {
                function.effects = static_cast<uint8>(function.effects | bit);
            }
        }
        function.firstTypeParameter = static_cast<uint32>(out_.typeParameters_.Size());
        const JsonValue typeParameters = ArrayOf(object, "typeParameters");
        for (usize index = 0; index < typeParameters.Size() && error_ == ErrorCode::None; ++index) {
            const JsonValue element = typeParameters.At(index);
            if (!element.IsObject()) {
                ReportAt(element, "Each type parameter must be an object.");
                continue;
            }

            constexpr StringView RequiredName[] = {"name"};
            CheckKeys(element, "type parameter", TypeParameterKeys, RequiredName);
            SchemaTypeParameter parameter;
            parameter.span =
                element.Find("name").IsValid() ? SpanOf(element.Find("name")) : SpanOf(element);
            if (ReadName(element, "name", false, parameter.name)) {
                for (uint32 existing = function.firstTypeParameter;
                     existing < out_.typeParameters_.Size(); ++existing) {
                    if (out_.Text(out_.typeParameters_[existing].name) ==
                        out_.Text(parameter.name)) {
                        ReportNamed(parameter.span, "Type parameter", out_.Text(parameter.name),
                                    " is declared twice.");
                        break;
                    }
                }
            }

            if (element.Find("constraint").IsValid()) {
                parameter.constraintSpan = SpanOf(element.Find("constraint"));
                ReadName(element, "constraint", false, parameter.constraint);
            }

            if (auto pushed = out_.typeParameters_.TryPushBack(parameter); !pushed) {
                error_ = pushed.Error();
            }
        }

        function.typeParameterCount =
            static_cast<uint32>(out_.typeParameters_.Size()) - function.firstTypeParameter;
        ReadParameters(object, true, function.firstParameter, function.parameterCount);
        if (object.Find("returns").IsValid()) {
            function.returnsSpan = SpanOf(object.Find("returns"));
        }

        ReadTypeNotation(object, "returns", function.returns);
        if (auto pushed = out_.functions_.TryPushBack(function); !pushed) {
            error_ = pushed.Error();
        }
    }

    void ReadEvent(JsonValue object) noexcept {
        if (!object.IsObject()) {
            ReportAt(object, "Each event must be an object.");
            return;
        }

        constexpr StringView Required[] = {"name", "id"};
        CheckKeys(object, "event", EventKeys, Required);
        SchemaEvent event;
        event.span = object.Find("name").IsValid() ? SpanOf(object.Find("name")) : SpanOf(object);
        if (ReadName(object, "name", false, event.name)) {
            UseMemberName(event.name, event.span);
        }

        ReadId(object, event.id, event.idSpan);
        const JsonValue publisher = object.Find("publisher");
        if (publisher.IsValid()) {
            if (publisher.IsString() && publisher.GetString() == "server") {
                event.publisher = SchemaPublisher::Server;
            } else if (publisher.IsString() && publisher.GetString() == "owner") {
                event.publisher = SchemaPublisher::Owner;
            } else if (!publisher.IsString() || publisher.GetString() != "all") {
                ReportAt(publisher, "'publisher' must be \"server\", \"owner\" or \"all\".");
            }
        }

        ReadParameters(object, false, event.firstParameter, event.parameterCount);
        if (auto pushed = out_.events_.TryPushBack(event); !pushed) {
            error_ = pushed.Error();
        }
    }

    // 한 파일 안의 타입 이름 중복이다(원문에서 뒤에 나온 쪽마다 하나).
    void CheckDuplicateTypeNames() noexcept {
        if (error_ != ErrorCode::None) {
            return;
        }

        const BindingSchema& schema = out_;
        algo::Sort(
            typeNames_.View(), [&schema](const NameUse& left, const NameUse& right) noexcept {
                const int order =
                    StringView::CompareBytes(schema.Text(left.name), schema.Text(right.name));
                return order != 0 ? order < 0 : left.span.begin < right.span.begin;
            });
        for (usize index = 1; index < typeNames_.Size(); ++index) {
            if (out_.Text(typeNames_[index].name) == out_.Text(typeNames_[index - 1].name)) {
                ReportNamed(typeNames_[index].span, "Type", out_.Text(typeNames_[index].name),
                            " is declared twice in this schema.");
            }
        }
    }

    // 한 파일 안의 ID 중복이다(원문에서 뒤에 나온 쪽마다 하나).
    void CheckDuplicateIds() noexcept {
        if (error_ != ErrorCode::None) {
            return;
        }

        const BindingSchema& schema = out_;
        algo::Sort(ids_.View(), [&schema](const IdUse& left, const IdUse& right) noexcept {
            const int order = StringView::CompareBytes(schema.Text(left.id), schema.Text(right.id));
            return order != 0 ? order < 0 : left.span.begin < right.span.begin;
        });
        for (usize index = 1; index < ids_.Size(); ++index) {
            if (out_.Text(ids_[index].id) == out_.Text(ids_[index - 1].id)) {
                Report(ids_[index].span, "This ID is used twice in the binding schema.",
                       "TypeId, FieldId and FunctionId values are unique.");
            }
        }
    }

    const SourceFile& file_;
    SourceId id_;
    DiagnosticList& diagnostics_;
    BindingSchema& out_;
    Allocator& allocator_;
    Array<IdUse> ids_;
    Array<NameUse> typeNames_;
    Array<NameUse> memberNames_;
    bool valid_ = true;
    ErrorCode error_ = ErrorCode::None;
};

} // namespace detail

Result<bool> ReadBindingSchema(const SourceSet& sources, SourceId id, DiagnosticList& diagnostics,
                               BindingSchema& out) noexcept {
    if (!sources.Get(id).IsUsable()) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    detail::SchemaReader reader(sources, id, diagnostics, out);
    return reader.Run();
}

} // namespace rocks::sparks
