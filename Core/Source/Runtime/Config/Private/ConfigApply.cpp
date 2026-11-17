#include "ConfigState.h"

#include <Core/Algorithms.h>
#include <Core/ScopeExit.h>
#include <Core/Text/StringBuilder.h>
#include <Object/PropertyPath.h>
#include <ReflectionText/TextJson.h>

// 입력 하나를 설정 struct 값에 적용한다(Docs/Design/ConfigSystem.ko.md 4절).
//
// 섹션 JSON은 ReflectionText ImportJson(struct 재귀 병합·Map 키별 병합·배열 통째 교체)으로 읽는다.
// ImportJson이 모르는 두 가지 — 배열·Set 필드 자리의 연산 객체(4.2절)와 문자열 값의 경로
// 이름(4.3절) — 는 이 모듈이 import 전에 처리한다: 섹션을 스키마와 함께 걸으며 JSON을 다시
// 쓰고(연산 객체는 이전 층까지의 값에 연산을 적용한 배열로, 경로 이름은 펼친 문자열로), 다시 쓴
// 노드마다 원문 위치를 기록해 import 진단의 줄·열을 원문 위치로 되돌린다. 다시 쓸 것이 없으면 원문
// 문서를 그대로 import한다.
//
// 배열 연산의 원소 비교는 리플렉션 값의 정규 JSON 표기(ExportJson) 동등성이다: 이전 값은 현재
// 값을 export한 배열이고, 연산 목록은 기본값 struct에 그 목록만 import한 뒤 다시 export해 같은
// 정규 표기로 만든다(enum 도구 이름 대소문자·실수 표기·Name NFC 등이 같은 값이면 같은 텍스트가
// 된다). 그래서 이 단계는 ReflectionText를 고치지 않는다.
namespace rocks::config::detail {

namespace {

using namespace reflection_text;

// 다시 쓴 JSON과 그 노드(전위 순서)마다의 원문 위치다.
struct WriteSession {
    explicit WriteSession(Allocator& allocator) noexcept
        : text(allocator), writer(text, JsonNumberMode::Preserve), origins(allocator) {
    }

    StringBuilder<256> text;
    JsonWriter writer;
    Array<JsonPosition> origins;
    bool changed = false;
};

// 다시 쓴 문서의 노드 위치(byte offset) → 원문 위치다(offset 오름차순).
struct OffsetOrigin {
    uint64 offset;
    JsonPosition origin;
};

// OffsetOrigin을 byte offset으로 찾는 비교다(algo::BinarySearch는 두 방향을 모두 부른다).
struct OffsetLess {
    bool operator()(const OffsetOrigin& left, uint64 right) const noexcept {
        return left.offset < right;
    }

    bool operator()(uint64 left, const OffsetOrigin& right) const noexcept {
        return left < right.offset;
    }
};

// 섹션 루트에서 배열 연산 필드까지의 struct 멤버 사슬 한 칸이다.
struct ChainStep {
    StructView owner;
    usize index;
};

Span<const char> ToSpan(StringView text) noexcept {
    return Span<const char>(text.Data(), text.ByteCount());
}

StringView ToView(Span<const char> text) noexcept {
    return StringView(text.Data(), text.Size());
}

// value 서브트리의 노드를 전위 순서로 걷는다(반복형 — 깊이는 JSON 읽기 한도 안이다).
template <class Visit>
Result<void> VisitPreorder(Allocator& allocator, JsonValue value, Visit&& visit) noexcept {
    Array<JsonValue> stack(allocator);
    if (auto pushed = stack.TryPushBack(value); !pushed) {
        return pushed;
    }

    while (!stack.IsEmpty()) {
        auto popped = stack.TryPopBack();
        if (!popped) {
            return Result<void>::Failure(popped.Error());
        }

        const JsonValue node = popped.Value();
        if (auto visited = visit(node); !visited) {
            return visited;
        }

        const usize count = node.IsArray() || node.IsObject() ? node.Size() : 0;
        for (usize child = count; child > 0; --child) {
            const JsonValue next = node.IsArray() ? node.At(child - 1) : node.ValueAt(child - 1);
            if (auto pushed = stack.TryPushBack(next); !pushed) {
                return pushed;
            }
        }
    }

    return Result<void>::Success();
}

// value 서브트리 노드 수만큼 위치를 더한다(fixed가 있으면 모두 그 위치, 없으면 각 노드의 원문
// 위치).
Result<void> PushOrigins(Allocator& allocator, JsonValue value, const JsonPosition* fixed,
                         Array<JsonPosition>& origins) noexcept {
    return VisitPreorder(allocator, value, [&](JsonValue node) noexcept {
        return origins.TryPushBack(fixed != nullptr ? *fixed : node.Position());
    });
}

// 섹션 하나에 입력 하나를 적용하는 일이다. 한 입력의 한 섹션마다 만든다.
class SectionApplier {
public:
    SectionApplier(ConfigState& state, SectionState& section, void* storage,
                   const ConfigEntry& entry) noexcept
        : state_(state), section_(section), storage_(storage), entry_(entry),
          allocator_(state.allocator), keep_(state.allocator), path_(state.allocator) {
    }

    Result<bool> ApplyDocument(JsonValue object, Array<String>& paths) noexcept;
    Result<bool> ApplyAssignment(Array<String>& paths) noexcept;

private:
    Result<void> Diagnose(ConfigDiagnosticCode code, const JsonPosition* position,
                          StringView message) noexcept;

    Result<void> WriteCopy(WriteSession& session, JsonValue value) noexcept;
    Result<void> WriteStruct(WriteSession& session, JsonValue value, StructView view, bool nested,
                             Array<ChainStep>& chain) noexcept;
    Result<void> WriteField(WriteSession& session, JsonValue value, StructView owner, usize index,
                            bool nested, Array<ChainStep>& chain) noexcept;
    Result<void> WriteInner(WriteSession& session, JsonValue value, ValueKind kind,
                            StructView target) noexcept;
    Result<void> WriteList(WriteSession& session, JsonValue list, ValueKind kind,
                           StructView target) noexcept;
    Result<void> WriteMap(WriteSession& session, JsonValue map, ValueKind kind,
                          StructView target) noexcept;
    Result<void> WriteString(WriteSession& session, JsonValue value) noexcept;
    Result<void> WriteOperation(WriteSession& session, JsonValue operation,
                                Array<ChainStep>& chain) noexcept;

    Result<bool> ExpandTokens(StringView text, const JsonPosition* position, String& out) noexcept;
    Result<JsonValue> CurrentArray(Span<const ChainStep> chain) noexcept;
    Result<JsonValue> CanonicalList(Span<const ChainStep> chain, JsonValue list) noexcept;
    Result<JsonValue> Navigate(JsonValue root, Span<const ChainStep> chain) noexcept;
    Result<JsonValue> ExportValue(void* storage) noexcept;

    Result<bool> ImportMapped(void* storage, JsonValue root, const WriteSession* session,
                              JsonValue original, Array<String>* paths) noexcept;
    Result<bool> FinishPrepared(Result<PreparedAssignments> prepared, TextDiagnostics& diagnostics,
                                const Array<OffsetOrigin>* map, JsonValue original,
                                Array<String>* paths) noexcept;

    Result<void> PushPath(StringView step) noexcept;
    void PopPath(usize length) noexcept;

    ConfigState& state_;
    SectionState& section_;
    void* storage_;
    const ConfigEntry& entry_;
    Allocator& allocator_;
    // 배열 연산의 정규 표기 문서(export 결과)다. 다시 쓴 문서가 그 값을 복사할 때까지 산다.
    Array<UniquePtr<JsonDocument>> keep_;
    JsonValue current_{};
    // 진단 경로 텍스트(섹션 안 도구 이름 경로)다.
    Array<char> path_;
    bool failed_ = false;
};

Result<void> SectionApplier::Diagnose(ConfigDiagnosticCode code, const JsonPosition* position,
                                      StringView message) noexcept {
    failed_ = true;
    return Report(state_, code, ConfigSeverity::Error, entry_.layer, entry_.sourceName.View(),
                  position, StringView(path_.Data(), path_.Size()), message);
}

Result<void> SectionApplier::PushPath(StringView step) noexcept {
    if (!path_.IsEmpty()) {
        if (auto dot = path_.TryPushBack('.'); !dot) {
            return dot;
        }
    }

    return path_.TryAppend(ToSpan(step));
}

void SectionApplier::PopPath(usize length) noexcept {
    static_cast<void>(path_.TryResize(length)); // 줄이기는 할당하지 않는다
}

Result<void> SectionApplier::WriteCopy(WriteSession& session, JsonValue value) noexcept {
    if (auto written = session.writer.Value(value); !written) {
        return written;
    }

    return PushOrigins(allocator_, value, nullptr, session.origins);
}

Result<void> SectionApplier::WriteStruct(WriteSession& session, JsonValue value, StructView view,
                                         bool nested, Array<ChainStep>& chain) noexcept {
    // 객체가 아니면 그대로 쓴다(ImportJson이 형식 진단을 원문 위치로 낸다).
    if (!value.IsObject()) {
        return WriteCopy(session, value);
    }

    if (auto begun = session.writer.BeginObject(); !begun) {
        return begun;
    }

    if (auto pushed = session.origins.TryPushBack(value.Position()); !pushed) {
        return pushed;
    }

    const auto& fields = view.Schema().fields;
    for (usize member = 0; member < value.Size(); ++member) {
        const StringView key = value.KeyAt(member);
        const JsonValue child = value.ValueAt(member);
        if (auto keyed = session.writer.Key(key); !keyed) {
            return keyed;
        }

        const JsonPosition at = child.Position();
        if (key.StartsWith("$", StringComparison::CaseSensitive)) {
            if (auto reported = Diagnose(ConfigDiagnosticCode::MisplacedOperation, &at,
                                         "array operations ($add, $append, $remove, $clear) "
                                         "replace an array or set field value only");
                !reported) {
                return reported;
            }
        }

        usize found = IndexNone;
        for (usize index = 0; index < fields.Size(); ++index) {
            if (EqualsIgnoringAsciiCase(EffectiveToolName(fields[index]), ToSpan(key))) {
                found = index;
                break;
            }
        }

        // 모르는 키는 그대로 쓴다(ImportJson이 UnknownProperty를 원문 위치로 낸다).
        if (found == IndexNone) {
            if (auto copied = WriteCopy(session, child); !copied) {
                return copied;
            }

            continue;
        }

        const usize length = path_.Size();
        if (auto pushed = PushPath(ToView(EffectiveToolName(fields[found]))); !pushed) {
            return pushed;
        }

        Result<void> written = Result<void>::Success();
        if (!nested) {
            written = chain.TryPushBack(ChainStep{view, found});
        }

        if (written) {
            written = WriteField(session, child, view, found, nested, chain);
        }

        if (!nested && written) {
            static_cast<void>(chain.TryPopBack());
        }

        PopPath(length);
        if (!written) {
            return written;
        }
    }

    return session.writer.EndObject();
}

Result<void> SectionApplier::WriteField(WriteSession& session, JsonValue value, StructView owner,
                                        usize index, bool nested,
                                        Array<ChainStep>& chain) noexcept {
    const FieldSchema& field = owner.Schema().fields[index];
    const JsonPosition at = value.Position();
    switch (field.value.kind) {
    case ValueKind::Struct:
        return WriteStruct(session, value, owner.FieldStruct(index), nested, chain);
    case ValueKind::Array:
    case ValueKind::Set:
        if (value.IsObject()) {
            if (nested) {
                if (auto reported =
                        Diagnose(ConfigDiagnosticCode::UnsupportedOperation, &at,
                                 "array operations are supported only on fields reached through "
                                 "struct members (not inside array elements, map values or "
                                 "optionals)");
                    !reported) {
                    return reported;
                }

                return WriteCopy(session, value);
            }

            return WriteOperation(session, value, chain);
        }

        if (value.IsArray()) {
            return WriteList(session, value, field.element.kind, owner.FieldStruct(index));
        }

        return WriteCopy(session, value);
    case ValueKind::StaticArray:
        if (value.IsObject()) {
            if (auto reported =
                    Diagnose(ConfigDiagnosticCode::InvalidArrayOperation, &at,
                             "array operations apply to array and set fields only (a static "
                             "array has a fixed length)");
                !reported) {
                return reported;
            }

            return WriteCopy(session, value);
        }

        if (value.IsArray()) {
            return WriteList(session, value, field.element.kind, owner.FieldStruct(index));
        }

        return WriteCopy(session, value);
    case ValueKind::Map:
        if (value.IsObject()) {
            return WriteMap(session, value, field.element.kind, owner.FieldStruct(index));
        }

        return WriteCopy(session, value);
    case ValueKind::Optional:
        if (value.IsNull()) {
            return WriteCopy(session, value);
        }

        return WriteInner(session, value, field.element.kind, owner.FieldStruct(index));
    case ValueKind::Utf8String:
        return WriteString(session, value);
    default:
        return WriteCopy(session, value);
    }
}

Result<void> SectionApplier::WriteInner(WriteSession& session, JsonValue value, ValueKind kind,
                                        StructView target) noexcept {
    if (kind == ValueKind::Struct) {
        Array<ChainStep> unused(allocator_);
        return WriteStruct(session, value, target, true, unused);
    }

    if (kind == ValueKind::Utf8String) {
        return WriteString(session, value);
    }

    return WriteCopy(session, value);
}

Result<void> SectionApplier::WriteList(WriteSession& session, JsonValue list, ValueKind kind,
                                       StructView target) noexcept {
    if (!list.IsArray()) {
        return WriteCopy(session, list);
    }

    if (auto begun = session.writer.BeginArray(); !begun) {
        return begun;
    }

    if (auto pushed = session.origins.TryPushBack(list.Position()); !pushed) {
        return pushed;
    }

    for (usize element = 0; element < list.Size(); ++element) {
        if (auto written = WriteInner(session, list.At(element), kind, target); !written) {
            return written;
        }
    }

    return session.writer.EndArray();
}

Result<void> SectionApplier::WriteMap(WriteSession& session, JsonValue map, ValueKind kind,
                                      StructView target) noexcept {
    if (auto begun = session.writer.BeginObject(); !begun) {
        return begun;
    }

    if (auto pushed = session.origins.TryPushBack(map.Position()); !pushed) {
        return pushed;
    }

    for (usize member = 0; member < map.Size(); ++member) {
        const StringView key = map.KeyAt(member);
        const JsonValue child = map.ValueAt(member);
        if (key.StartsWith("$", StringComparison::CaseSensitive)) {
            const JsonPosition at = child.Position();
            if (auto reported = Diagnose(ConfigDiagnosticCode::MisplacedOperation, &at,
                                         "'$' keys are reserved for array operations");
                !reported) {
                return reported;
            }
        }

        if (auto keyed = session.writer.Key(key); !keyed) {
            return keyed;
        }

        if (auto written = WriteInner(session, child, kind, target); !written) {
            return written;
        }
    }

    return session.writer.EndObject();
}

Result<void> SectionApplier::WriteString(WriteSession& session, JsonValue value) noexcept {
    if (!value.IsString()) {
        return WriteCopy(session, value);
    }

    const JsonPosition at = value.Position();
    String expanded(allocator_);
    auto changed = ExpandTokens(value.GetString(), &at, expanded);
    if (!changed) {
        return Result<void>::Failure(changed.Error());
    }

    if (!changed.Value()) {
        return WriteCopy(session, value);
    }

    session.changed = true;
    if (auto written = session.writer.String(expanded.View()); !written) {
        return written;
    }

    return session.origins.TryPushBack(at);
}

// 4.3절: `{이름}`(이름은 [A-Za-z0-9_]+)을 표의 디렉터리 값으로 바꾼다. 이름 문법이 아닌 `{`는
// 글자 그대로다. 반환: 바꾼 것이 있으면 true(out에 결과). 표에 없거나 값이 빈 이름은 진단을 남기고
// 글자 그대로 둔다.
Result<bool> SectionApplier::ExpandTokens(StringView text, const JsonPosition* position,
                                          String& out) noexcept {
    bool changed = false;
    usize copied = 0;
    const char* data = text.Data();
    const usize size = text.ByteCount();
    for (usize open = 0; open < size; ++open) {
        if (data[open] != '{') {
            continue;
        }

        usize close = open + 1;
        while (close < size && ((data[close] >= 'A' && data[close] <= 'Z') ||
                                (data[close] >= 'a' && data[close] <= 'z') ||
                                (data[close] >= '0' && data[close] <= '9') || data[close] == '_')) {
            ++close;
        }

        if (close == open + 1 || close >= size || data[close] != '}') {
            continue;
        }

        const StringView name(data + open + 1, close - open - 1);
        usize token = IndexNone;
        for (usize index = 0;
             index < sizeof(ConfigPathTokenNames) / sizeof(ConfigPathTokenNames[0]); ++index) {
            if (ConfigPathTokenNames[index] == name) {
                token = index;
                break;
            }
        }

        if (token == IndexNone || state_.directories[token].IsEmpty()) {
            StringBuilder<128> message(allocator_);
            static_cast<void>(
                message.TryAppend(token == IndexNone ? "unknown path name '" : "path name '"));
            static_cast<void>(message.TryAppend(StringView(data + open, close + 1 - open)));
            static_cast<void>(message.TryAppend(
                token == IndexNone
                    ? "' (known: EngineDir, ProjectDir, UserDataDir, SavedDir in braces)"
                    : "' has no value in this config context"));
            if (auto status = message.Status(); !status) {
                return Result<bool>::Failure(status.Error());
            }

            if (auto reported =
                    Diagnose(ConfigDiagnosticCode::InvalidPathToken, position, message.View());
                !reported) {
                return Result<bool>::Failure(reported.Error());
            }

            continue;
        }

        if (!changed) {
            out.Clear();
            changed = true;
        }

        if (auto appended = out.TryAppend(StringView(data + copied, open - copied)); !appended) {
            return Result<bool>::Failure(appended.Error());
        }

        if (auto appended = out.TryAppend(state_.directories[token].View()); !appended) {
            return Result<bool>::Failure(appended.Error());
        }

        copied = close + 1;
        open = close;
    }

    if (changed) {
        if (auto appended = out.TryAppend(StringView(data + copied, size - copied)); !appended) {
            return Result<bool>::Failure(appended.Error());
        }
    }

    return Result<bool>::Success(changed);
}

Result<JsonValue> SectionApplier::ExportValue(void* storage) noexcept {
    StringBuilder<256> text(allocator_);
    JsonWriter writer(text);
    const TextCodecOptions options = state_.CodecOptions();
    const AssignmentTarget target =
        AssignmentTarget::Value(state_.objects, state_.access, section_.lease, storage, allocator_);
    if (auto exported = ExportJson(target, writer, options); !exported) {
        return Result<JsonValue>::Failure(exported.Error());
    }

    if (auto finished = writer.Finish(); !finished) {
        return Result<JsonValue>::Failure(finished.Error());
    }

    auto document = UniquePtr<JsonDocument>::TryMake(allocator_, allocator_);
    if (!document) {
        return Result<JsonValue>::Failure(document.Error());
    }

    if (!document.Value()->Parse(text.View()).Succeeded()) {
        return Result<JsonValue>::Failure(ErrorCode::InvalidArgument);
    }

    const JsonValue root = document.Value()->Root();
    if (auto kept = keep_.TryPushBack(Move(document.Value())); !kept) {
        return Result<JsonValue>::Failure(kept.Error());
    }

    return Result<JsonValue>::Success(root);
}

Result<JsonValue> SectionApplier::Navigate(JsonValue root, Span<const ChainStep> chain) noexcept {
    JsonValue node = root;
    for (const ChainStep& step : chain) {
        node = node.Find(ToView(EffectiveToolName(step.owner.Schema().fields[step.index])));
    }

    if (!node.IsArray()) {
        return Result<JsonValue>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<JsonValue>::Success(node);
}

Result<JsonValue> SectionApplier::CurrentArray(Span<const ChainStep> chain) noexcept {
    if (!current_.IsValid()) {
        auto exported = ExportValue(storage_);
        if (!exported) {
            return exported;
        }

        current_ = exported.Value();
    }

    return Navigate(current_, chain);
}

// 연산 목록 list를 기본값 struct의 그 필드에 import했다가 다시 export해 정규 표기 배열로 만든다.
// 목록 원소의 진단(모르는 키·형식 등)은 원소의 원문 위치로 낸다. 진단을 남겼으면 무효 값이다.
Result<JsonValue> SectionApplier::CanonicalList(Span<const ChainStep> chain,
                                                JsonValue list) noexcept {
    void* scratch = section_.ops.create(allocator_);
    if (scratch == nullptr) {
        return Result<JsonValue>::Failure(ErrorCode::OutOfMemory);
    }

    auto release = MakeScopeExit([&]() noexcept { section_.ops.destroy(allocator_, scratch); });
    const JsonPosition at = list.Position();
    WriteSession session(allocator_);
    for (const ChainStep& step : chain) {
        if (auto begun = session.writer.BeginObject(); !begun) {
            return Result<JsonValue>::Failure(begun.Error());
        }

        if (auto pushed = session.origins.TryPushBack(at); !pushed) {
            return Result<JsonValue>::Failure(pushed.Error());
        }

        const FieldSchema& field = step.owner.Schema().fields[step.index];
        if (auto keyed = session.writer.Key(ToView(EffectiveToolName(field))); !keyed) {
            return Result<JsonValue>::Failure(keyed.Error());
        }
    }

    const ChainStep& last = chain[chain.Size() - 1];
    const FieldSchema& field = last.owner.Schema().fields[last.index];
    if (auto written =
            WriteList(session, list, field.element.kind, last.owner.FieldStruct(last.index));
        !written) {
        return Result<JsonValue>::Failure(written.Error());
    }

    // 목록 원소의 `$` 키·경로 이름 진단이 있었으면 import하지 않는다.
    if (failed_) {
        return Result<JsonValue>::Success(JsonValue{});
    }

    for (usize step = 0; step < chain.Size(); ++step) {
        if (auto ended = session.writer.EndObject(); !ended) {
            return Result<JsonValue>::Failure(ended.Error());
        }
    }

    if (auto finished = session.writer.Finish(); !finished) {
        return Result<JsonValue>::Failure(finished.Error());
    }

    JsonDocument document(allocator_);
    if (!document.Parse(session.text.View()).Succeeded()) {
        return Result<JsonValue>::Failure(ErrorCode::InvalidArgument);
    }

    auto imported = ImportMapped(scratch, document.Root(), &session, list, nullptr);
    if (!imported) {
        return Result<JsonValue>::Failure(imported.Error());
    }

    if (!imported.Value()) {
        return Result<JsonValue>::Success(JsonValue{});
    }

    auto exported = ExportValue(scratch);
    if (!exported) {
        return exported;
    }

    return Navigate(exported.Value(), chain);
}

// 4.2절 연산 객체 하나를 이전 값에 적용한 배열로 쓴다. 적용 순서: $clear → $remove → $add·$append.
Result<void> SectionApplier::WriteOperation(WriteSession& session, JsonValue operation,
                                            Array<ChainStep>& chain) noexcept {
    const ChainStep& last = chain[chain.Size() - 1];
    const bool set = last.owner.Schema().fields[last.index].value.kind == ValueKind::Set;
    const JsonPosition at = operation.Position();
    bool clear = false;
    JsonValue remove{};
    JsonValue add{};
    JsonValue append{};
    bool valid = operation.Size() != 0;
    if (!valid) {
        if (auto reported = Diagnose(ConfigDiagnosticCode::InvalidArrayOperation, &at,
                                     "an array operation object needs at least one of $clear, "
                                     "$remove, $add, $append");
            !reported) {
            return reported;
        }
    }

    for (usize member = 0; member < operation.Size(); ++member) {
        const StringView key = operation.KeyAt(member);
        const JsonValue value = operation.ValueAt(member);
        const JsonPosition where = value.Position();
        StringView problem;
        if (key == "$clear") {
            if (value.IsBool() && value.GetBool()) {
                clear = true;
            } else {
                problem = "$clear takes true";
            }
        } else if (key == "$remove" || key == "$add" || key == "$append") {
            if (!value.IsArray()) {
                problem = "$remove, $add and $append take an array of elements";
            } else if (key == "$append" && set) {
                problem = "a set has no duplicates; use $add";
            } else if (key == "$remove") {
                remove = value;
            } else if (key == "$add") {
                add = value;
            } else {
                append = value;
            }
        } else {
            problem = "unknown array operation (known: $clear, $remove, $add, $append)";
        }

        if (!problem.IsEmpty()) {
            valid = false;
            if (auto reported =
                    Diagnose(ConfigDiagnosticCode::InvalidArrayOperation, &where, problem);
                !reported) {
                return reported;
            }
        }
    }

    if (!valid) {
        return WriteCopy(session, operation);
    }

    // 이전 값(정규 표기)과 연산 목록(정규 표기)을 구한다.
    JsonValue previous{};
    if (!clear) {
        auto exported = CurrentArray(chain.View());
        if (!exported) {
            return Result<void>::Failure(exported.Error());
        }

        previous = exported.Value();
    }

    JsonValue lists[3] = {remove, add, append};
    for (JsonValue& list : lists) {
        if (!list.IsValid()) {
            continue;
        }

        auto canonical = CanonicalList(chain.View(), list);
        if (!canonical) {
            return Result<void>::Failure(canonical.Error());
        }

        if (!canonical.Value().IsValid()) {
            return WriteCopy(session, operation);
        }

        list = canonical.Value();
    }

    // 결과 원소(정규 표기 값)와 그 원문 위치(이전 값은 연산 객체 위치, 목록 원소는 그 원소 위치).
    struct Element {
        JsonValue value;
        JsonPosition origin;
    };

    Array<Element> result(allocator_);
    if (previous.IsValid()) {
        for (usize index = 0; index < previous.Size(); ++index) {
            const JsonValue element = previous.At(index);
            bool removed = false;
            for (usize other = 0; lists[0].IsValid() && other < lists[0].Size() && !removed;
                 ++other) {
                removed = JsonEquals(element, lists[0].At(other));
            }

            if (!removed) {
                if (auto pushed = result.TryPushBack(Element{element, at}); !pushed) {
                    return pushed;
                }
            }
        }
    }

    for (usize index = 0; lists[1].IsValid() && index < lists[1].Size(); ++index) {
        const JsonValue element = lists[1].At(index);
        bool present = false;
        for (const Element& existing : result) {
            present = present || JsonEquals(existing.value, element);
        }

        if (!present) {
            if (auto pushed = result.TryPushBack(Element{element, add.At(index).Position()});
                !pushed) {
                return pushed;
            }
        }
    }

    for (usize index = 0; lists[2].IsValid() && index < lists[2].Size(); ++index) {
        if (auto pushed =
                result.TryPushBack(Element{lists[2].At(index), append.At(index).Position()});
            !pushed) {
            return pushed;
        }
    }

    session.changed = true;
    if (auto begun = session.writer.BeginArray(); !begun) {
        return begun;
    }

    if (auto pushed = session.origins.TryPushBack(at); !pushed) {
        return pushed;
    }

    for (const Element& element : result) {
        if (auto written = session.writer.Value(element.value); !written) {
            return written;
        }

        if (auto pushed = PushOrigins(allocator_, element.value, &element.origin, session.origins);
            !pushed) {
            return pushed;
        }
    }

    return session.writer.EndArray();
}

// root를 storage에 import한다. session이 있으면 root는 다시 쓴 문서이고 진단 위치를 원문으로
// 되돌린다(original은 원문 섹션 — 층 허용 진단의 위치를 찾는다). paths가 있으면 override 경로
// 텍스트를 더한다.
Result<bool> SectionApplier::ImportMapped(void* storage, JsonValue root,
                                          const WriteSession* session, JsonValue original,
                                          Array<String>* paths) noexcept {
    Array<OffsetOrigin> map(allocator_);
    if (session != nullptr) {
        usize next = 0;
        auto zipped = VisitPreorder(allocator_, root, [&](JsonValue node) noexcept {
            if (next >= session->origins.Size()) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            return map.TryPushBack(
                OffsetOrigin{node.Position().byteOffset, session->origins[next++]});
        });
        if (!zipped) {
            return Result<bool>::Failure(zipped.Error());
        }

        if (next != session->origins.Size()) {
            return Result<bool>::Failure(ErrorCode::InvalidArgument);
        }
    }

    TextDiagnostics diagnostics(allocator_);
    const TextCodecOptions options = state_.CodecOptions();
    const AssignmentTarget target =
        AssignmentTarget::Value(state_.objects, state_.access, section_.lease, storage, allocator_);
    return FinishPrepared(ImportJson(root, target, options, diagnostics), diagnostics,
                          session != nullptr ? &map : nullptr, original, paths);
}

Result<bool> SectionApplier::FinishPrepared(Result<PreparedAssignments> prepared,
                                            TextDiagnostics& diagnostics,
                                            const Array<OffsetOrigin>* map, JsonValue original,
                                            Array<String>* paths) noexcept {
    if (!prepared) {
        if (!diagnostics.HasErrors()) {
            return Result<bool>::Failure(prepared.Error());
        }

        failed_ = true;

        for (usize index = 0; index < diagnostics.Count(); ++index) {
            const TextDiagnostic item = diagnostics.Get(index);
            ConfigDiagnostic diagnostic;
            diagnostic.code = ConfigDiagnosticCode::FieldValue;
            diagnostic.textCode = item.code;
            diagnostic.layer = entry_.layer;
            diagnostic.source = entry_.sourceName.View();
            diagnostic.path = item.path;
            diagnostic.message = item.message;
            if (item.source.kind == TextSourceKind::Json) {
                diagnostic.position = item.source.position;
                if (map != nullptr) {
                    const usize found = algo::BinarySearch(
                        map->View(), item.source.position.byteOffset, OffsetLess{});
                    if (found != IndexNone) {
                        diagnostic.position = (*map)[found].origin;
                    }
                }
            }

            if (auto added = state_.diagnostics.Add(diagnostic); !added) {
                return Result<bool>::Failure(added.Error());
            }
        }

        return Result<bool>::Success(false);
    }

    // 층 허용(사용자 층의 UserConfig, 출시 규칙의 DevelopmentOnly)은 override 단위의 최상위 필드로
    // 본다.
    bool allowed = true;
    for (const PropertyPath& path : prepared.Value().OverridePaths()) {
        if (path.steps.IsEmpty()) {
            continue;
        }

        const StringView reason = CheckLayerAllowed(
            state_, entry_.layer, SettingsFlagsOf(section_, path.steps[0].fieldId));
        if (reason.IsEmpty()) {
            continue;
        }

        allowed = false;
        failed_ = true;
        String text(allocator_);
        static_cast<void>(FormatPropertyPath(section_.lease, path, text));
        const usize field = section_.lease.FieldIndex(path.steps[0].fieldId);
        JsonValue member{};
        if (original.IsObject() && field != IndexNone) {
            member = FindMemberIgnoringCase(
                original, ToView(EffectiveToolName(section_.lease.Schema().fields[field])));
        }

        const JsonPosition at = member.IsValid() ? member.Position() : JsonPosition{0, 0, 0};
        if (auto reported = Report(state_, ConfigDiagnosticCode::LayerNotAllowed,
                                   ConfigSeverity::Error, entry_.layer, entry_.sourceName.View(),
                                   member.IsValid() ? &at : nullptr, text.View(), reason);
            !reported) {
            return Result<bool>::Failure(reported.Error());
        }
    }

    if (!allowed) {
        return Result<bool>::Success(false);
    }

    auto published = prepared.Value().Publish();
    if (!published) {
        return Result<bool>::Failure(published.Error());
    }

    if (paths != nullptr) {
        for (const PropertyPath& path : published.Value().OverridePaths()) {
            String text(allocator_);
            if (auto formatted = FormatPropertyPath(section_.lease, path, text); !formatted) {
                return Result<bool>::Failure(formatted.Error());
            }

            if (auto pushed = paths->TryPushBack(Move(text)); !pushed) {
                return Result<bool>::Failure(pushed.Error());
            }
        }
    }

    return Result<bool>::Success(true);
}

Result<bool> SectionApplier::ApplyDocument(JsonValue object, Array<String>& paths) noexcept {
    WriteSession session(allocator_);
    Array<ChainStep> chain(allocator_);
    if (auto written = WriteStruct(session, object, section_.lease.Struct(), false, chain);
        !written) {
        return Result<bool>::Failure(written.Error());
    }

    if (failed_) {
        return Result<bool>::Success(false);
    }

    if (!session.changed) {
        return ImportMapped(storage_, object, nullptr, object, &paths);
    }

    if (auto finished = session.writer.Finish(); !finished) {
        return Result<bool>::Failure(finished.Error());
    }

    JsonDocument rewritten(allocator_);
    if (!rewritten.Parse(session.text.View()).Succeeded()) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    return ImportMapped(storage_, rewritten.Root(), &session, object, &paths);
}

Result<bool> SectionApplier::ApplyAssignment(Array<String>& paths) noexcept {
    if (auto pushed = path_.TryAppend(ToSpan(entry_.path.View())); !pushed) {
        return Result<bool>::Failure(pushed.Error());
    }

    String expanded(allocator_);
    auto changed = ExpandTokens(entry_.value.View(), nullptr, expanded);
    if (!changed) {
        return Result<bool>::Failure(changed.Error());
    }

    if (failed_) {
        return Result<bool>::Success(false);
    }

    const TextAssignment assignment{AssignmentKind::Set, entry_.path.View(),
                                    changed.Value() ? expanded.View() : entry_.value.View(), 0};
    TextDiagnostics diagnostics(allocator_);
    const TextCodecOptions options = state_.CodecOptions();
    const AssignmentTarget target = AssignmentTarget::Value(state_.objects, state_.access,
                                                            section_.lease, storage_, allocator_);
    return FinishPrepared(PrepareAssignments(target, Span<const TextAssignment>(&assignment, 1),
                                             options, diagnostics),
                          diagnostics, nullptr, JsonValue{}, &paths);
}

} // namespace

JsonValue FindMemberIgnoringCase(JsonValue object, StringView key) noexcept {
    if (!object.IsObject()) {
        return JsonValue{};
    }

    for (usize member = 0; member < object.Size(); ++member) {
        if (EqualsIgnoringAsciiCase(ToSpan(object.KeyAt(member)), ToSpan(key))) {
            return object.ValueAt(member);
        }
    }

    return JsonValue{};
}

Result<bool> ApplyEntryToSection(ConfigState& state, SectionState& section, void* storage,
                                 const ConfigEntry& entry, Array<String>& paths) noexcept {
    SectionApplier applier(state, section, storage, entry);
    if (entry.kind == EntryKind::Assignment) {
        return applier.ApplyAssignment(paths);
    }

    const JsonValue object = FindMemberIgnoringCase(entry.document->Root(), section.section.View());
    if (!object.IsValid()) {
        return Result<bool>::Success(true);
    }

    return applier.ApplyDocument(object, paths);
}

} // namespace rocks::config::detail
