#include "GraphTextInternal.h"

#include <Core/Encoding/Base64.h>
#include <Core/Guid.h>
#include <ObjectSerialization/StructRecord.h>
#include <Platform/Fatal.h>
#include <ReflectionText/TextJson.h>
#include <Serialization/Record.h>

// 레코드 값(PropertyRecord)과 JSON 사이 변환이다(Docs/Design/NodeGraph.ko.md 9.4절 "레코드 값"):
// 리플렉션 텍스트는 독립 struct Record(Docs/Design/ReflectionExtensions.ko.md 26절)로 값을 만든 뒤
// ExportJson·ImportJson을 거친다. 텍스트로 쓰기 전에 그 텍스트를 다시 읽어 같은 바이트가 되는지
// 확인하고, 해석할 수 없거나 왕복이 다르면(ReadOnly·출력 제외 필드, 표기 없는 종류 등) base64
// 바이트로 쓴다. 할당 실패만 쓰기 전체를 실패시킨다.
namespace rocks::nodegraph::text_detail {
namespace {

using reflection_text::TextUse;

// 값 블록 태그다("NGRV").
constexpr uint32 RecordValueTag = 0x4e475256u;

// Guid 표기를 받을 고정 버퍼 크기다(36자 + NUL).
constexpr usize GuidBufferBytes = GuidTextBytes + 1;

// base64 4글자가 담는 바이트 수다(RFC 4648).
constexpr usize Base64GroupChars = 4;
constexpr usize Base64GroupBytes = 3;

// lease 타입의 기본 생성 값 하나다(ObjectAllocator — 모든 경로에서 소멸·반환).
class ValueBlock {
public:
    ValueBlock(const ValueBlock&) = delete;
    ValueBlock& operator=(const ValueBlock&) = delete;

    ValueBlock(const TypeLease& lease, Allocator& allocator) noexcept
        : ops_(lease.Struct().StructOps()), allocator_(allocator) {
    }

    ~ValueBlock() {
        if (memory_) {
            ops_.destroy(memory_);
            if (!allocator_.Deallocate(memory_)) {
                platform::Fatal("Graph record value block ownership mismatch");
            }
        }
    }

    Result<void*> Create() noexcept {
        if (ops_.construct == nullptr || ops_.destroy == nullptr) {
            return Result<void*>::Failure(ErrorCode::Unsupported);
        }

        auto memory = allocator_.Allocate(ops_.size, ops_.alignment, RecordValueTag);
        if (!memory) {
            return Result<void*>::Failure(memory.Error());
        }

        ops_.construct(memory.Value(), allocator_);
        memory_ = memory.Value();
        return Result<void*>::Success(memory_);
    }

private:
    const RuntimeStructOps& ops_;
    Allocator& allocator_;
    void* memory_ = nullptr;
};

// 해석 단계의 오류를 가른다: 할당 실패는 그대로 전파하고, 그 밖은 "텍스트로 쓸 수 없음"(바이트로
// 쓴다)이다.
Result<bool> Unwritable(ErrorCode error) noexcept {
    if (error == ErrorCode::OutOfMemory) {
        return Result<bool>::Failure(error);
    }

    return Result<bool>::Success(false);
}

// type 키의 문자열이다: 도구 이름이 있으면 그것, 없으면 struct Guid 표기.
Result<void> WriteTypeName(const TypeLease& lease, JsonWriter& writer) noexcept {
    const Span<const char> toolName = lease.Schema().toolName;
    if (!toolName.IsEmpty()) {
        return writer.String(StringView(toolName.Data(), toolName.Size()));
    }

    const GuidText text = FormatGuidText(lease.Schema().id);
    return writer.String(text.View());
}

Result<void> WriteRecordBytes(const PropertyRecord& record, JsonWriter& writer) noexcept {
    auto size = Base64EncodedSize(record.bytes.Size());
    if (!size) {
        return Result<void>::Failure(size.Error());
    }

    Array<char> encoded(record.bytes.GetAllocator());
    if (auto resized = encoded.TryResize(size.Value()); !resized) {
        return resized;
    }

    auto written = TryBase64Encode(record.bytes.View(), encoded.View());
    if (!written) {
        return Result<void>::Failure(written.Error());
    }

    if (auto begun = writer.BeginObject(); !begun) {
        return begun;
    }

    if (auto key = writer.Key("record"); !key) {
        return key;
    }

    if (auto value = writer.String(StringView(encoded.Data(), written.Value())); !value) {
        return value;
    }

    return writer.EndObject();
}

// fields(리플렉션 텍스트 JSON 객체)를 lease 타입의 기본 생성 값에 읽고 독립 Record로 out에 쓴다.
// 리플렉션 텍스트 진단이 있으면 InvalidArgument(내용은 diagnostics)다.
Result<void> ImportRecordText(GraphTextContext& context, const TypeLease& lease, JsonValue fields,
                              TextUse use, Array<uint8>& out,
                              reflection_text::TextDiagnostics& diagnostics) noexcept {
    ObjectRegistry& objects = context.objects;
    ValueBlock block(lease, objects.ObjectAllocator());
    auto storage = block.Create();
    if (!storage) {
        return Result<void>::Failure(storage.Error());
    }

    reflection_text::TextCodecOptions options;
    options.use = use;
    options.names = objects.Names();
    auto prepared = reflection_text::ImportJson(
        fields,
        reflection_text::AssignmentTarget::Value(objects, context.access, lease, storage.Value(),
                                                 objects.ObjectAllocator()),
        options, diagnostics);
    if (!prepared) {
        return Result<void>::Failure(prepared.Error());
    }

    if (auto published = prepared.Value().Publish(); !published) {
        return Result<void>::Failure(published.Error());
    }

    auto saved =
        SaveStructRecord(context.allocator, objects, context.access, lease, storage.Value());
    if (!saved) {
        return Result<void>::Failure(saved.Error());
    }

    out = Move(saved.Value());
    return Result<void>::Success();
}

// Record를 리플렉션 텍스트 JSON으로 만든다. 텍스트로 쓸 수 없으면(미등록·담을 수 없는 struct·손상·
// newer·표기 없는 종류, 다시 읽은 바이트가 다름) 거짓이다. 쓴 JSON은 document에 파싱해
// 둔다(writer가 실패 상태가 되지 않게 따로 만든다).
Result<bool> ExportRecordText(GraphTextContext& context, const PropertyRecord& record, TextUse use,
                              TypeLease& lease, JsonDocument& document) noexcept {
    ObjectRegistry& objects = context.objects;
    auto type = AcquireStructRecordType(objects.Types(), record.bytes.View());
    if (!type) {
        return Unwritable(type.Error());
    }

    ValueBlock block(type.Value(), objects.ObjectAllocator());
    auto value = block.Create();
    if (!value) {
        return Unwritable(value.Error());
    }

    auto loaded = LoadStructRecord(context.allocator, objects, context.access, record.bytes.View(),
                                   type.Value(), value.Value());
    if (!loaded) {
        return Unwritable(loaded.Error());
    }

    // 왕복 기준은 읽은 값을 다시 쓴 바이트다(옛 버전 Record는 마이그레이션된 현재 버전이 기준이다).
    auto canonical =
        SaveStructRecord(context.allocator, objects, context.access, type.Value(), value.Value());
    if (!canonical) {
        return Unwritable(canonical.Error());
    }

    StringBuilder<> text(context.allocator);
    JsonWriterOptions writerOptions;
    writerOptions.compact = true;
    JsonWriter json(text, writerOptions);
    reflection_text::TextCodecOptions options;
    options.use = use;
    options.names = objects.Names();
    auto exported = reflection_text::ExportJson(
        reflection_text::AssignmentTarget::Value(objects, context.access, type.Value(),
                                                 value.Value(), objects.ObjectAllocator()),
        json, options);
    if (!exported) {
        return Unwritable(exported.Error());
    }

    if (auto finished = json.Finish(); !finished) {
        return Unwritable(finished.Error());
    }

    if (!document.Parse(text.View()).Succeeded()) {
        return Result<bool>::Success(false);
    }

    // 텍스트를 다시 읽어 같은 바이트가 되어야 텍스트로 쓴다(ReadOnly·출력 제외 필드는 왕복이
    // 다르다).
    Array<uint8> again(context.allocator);
    reflection_text::TextDiagnostics diagnostics(context.allocator);
    if (auto imported =
            ImportRecordText(context, type.Value(), document.Root(), use, again, diagnostics);
        !imported) {
        return Unwritable(imported.Error());
    }

    if (again.Size() != canonical.Value().Size()) {
        return Result<bool>::Success(false);
    }

    for (usize index = 0; index < again.Size(); ++index) {
        if (again[index] != canonical.Value()[index]) {
            return Result<bool>::Success(false);
        }
    }

    lease = Move(type.Value());
    return Result<bool>::Success(true);
}

// "type" 문자열로 struct lease를 연다: Guid 표기면 struct ID, 아니면 도구 이름.
Result<TypeLease> AcquireTypeByText(TypeRegistry& types, StringView text) noexcept {
    if (auto id = TryParseGuid(text); id) {
        return types.AcquireValueType(id.Value(), TypeKind::Struct);
    }

    auto lease = types.AcquireByToolName(Span<const char>(text.Data(), text.ByteCount()));
    if (!lease) {
        return lease;
    }

    if (lease.Value().Kind() != TypeKind::Struct) {
        return Result<TypeLease>::Failure(ErrorCode::TypeMismatch);
    }

    return lease;
}

Result<void> Report(GraphTextDiagnostics& diagnostics, GraphTextCode code, JsonValue at,
                    StringView message) noexcept {
    if (auto added = diagnostics.Add(code, at.Position(), message); !added) {
        return added;
    }

    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

Result<RecordRead> Fail(GraphTextDiagnostics& diagnostics, GraphTextCode code, JsonValue at,
                        StringView message) noexcept {
    auto reported = Report(diagnostics, code, at, message);
    return Result<RecordRead>::Failure(reported.Error());
}

Result<RecordRead> ReadRecordBytes(JsonValue value, PropertyRecord& record,
                                   GraphTextDiagnostics& diagnostics) noexcept {
    if (!value.IsString()) {
        return Fail(diagnostics, GraphTextCode::InvalidValue, value, "record must be a string");
    }

    // 풀기 전에 글자 수로 상한을 본다(패딩까지 꽉 찬 그룹 기준 — 풀린 크기는 아래에서 다시 본다).
    const StringView text = value.GetString();
    const usize capacity = text.ByteCount() / Base64GroupChars * Base64GroupBytes;
    if (capacity > MaxPropertyRecordBytes + Base64GroupBytes) {
        return Fail(diagnostics, GraphTextCode::InvalidValue, value, "record is too large");
    }

    Array<uint8> bytes(record.bytes.GetAllocator());
    if (auto resized = bytes.TryResize(capacity); !resized) {
        return Result<RecordRead>::Failure(resized.Error());
    }

    auto decoded = TryBase64Decode(text, bytes.View());
    if (!decoded) {
        return Fail(diagnostics, GraphTextCode::InvalidValue, value, "record is not base64");
    }

    if (decoded.Value() > MaxPropertyRecordBytes) {
        return Fail(diagnostics, GraphTextCode::InvalidValue, value, "record is too large");
    }

    if (auto trimmed = bytes.TryRemoveAt(decoded.Value(), bytes.Size() - decoded.Value());
        !trimmed) {
        return Result<RecordRead>::Failure(trimmed.Error());
    }

    if (!ReadRecordHeader(bytes.View())) {
        return Fail(diagnostics, GraphTextCode::InvalidValue, value,
                    "record does not start with a record header");
    }

    record.bytes = Move(bytes);
    return Result<RecordRead>::Success(RecordRead::Resolved);
}

// ReflectionText 진단을 그래프 진단으로 옮긴다(이름·경로·메시지).
Result<void> ReportTextDiagnostics(GraphTextContext& context,
                                   const reflection_text::TextDiagnostics& text, JsonValue at,
                                   GraphTextDiagnostics& diagnostics) noexcept {
    for (usize index = 0; index < text.Count(); ++index) {
        const reflection_text::TextDiagnostic item = text.Get(index);
        StringBuilder<> message(context.allocator);
        message << reflection_text::TextDiagnosticCodeName(item.code) << " " << item.path;
        if (!item.message.IsEmpty()) {
            message << ": " << item.message;
        }

        if (auto status = message.Status(); !status) {
            return status;
        }

        JsonPosition position = item.source.position;
        if (item.source.kind != reflection_text::TextSourceKind::Json) {
            position = at.Position();
        }

        if (auto added = diagnostics.Add(GraphTextCode::RecordText, position, message.View());
            !added) {
            return added;
        }
    }

    if (text.Count() == 0) {
        return Report(diagnostics, GraphTextCode::RecordText, at, "record value was rejected");
    }

    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

} // namespace

GuidText FormatGuidText(Guid value) noexcept {
    char buffer[GuidBufferBytes] = {};
    FixedStringBuilder builder(Span<char>(buffer, GuidBufferBytes));
    if (!Formatter<Guid>::Format(builder, value, StringView()) ||
        builder.ByteCount() != GuidTextBytes) {
        platform::Fatal("Guid text formatting failed");
    }

    GuidText text;
    for (usize index = 0; index < GuidTextBytes; ++index) {
        text.bytes[index] = buffer[index];
    }

    return text;
}

Result<void> WriteRecordValue(GraphTextContext& context, const PropertyRecord& record, TextUse use,
                              bool records, JsonWriter& writer) noexcept {
    if (record.bytes.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!records) {
        JsonDocument document(context.allocator);
        TypeLease lease;
        auto exported = ExportRecordText(context, record, use, lease, document);
        if (!exported) {
            return Result<void>::Failure(exported.Error());
        }

        if (exported.Value()) {
            if (auto begun = writer.BeginObject(); !begun) {
                return begun;
            }

            if (auto key = writer.Key("type"); !key) {
                return key;
            }

            if (auto name = WriteTypeName(lease, writer); !name) {
                return name;
            }

            if (auto key = writer.Key("value"); !key) {
                return key;
            }

            if (auto value = writer.Value(document.Root()); !value) {
                return value;
            }

            return writer.EndObject();
        }
    }

    return WriteRecordBytes(record, writer);
}

Result<RecordRead> ReadRecordValue(GraphTextContext& context, JsonValue value, TextUse use,
                                   PropertyRecord& record, String& unresolvedType,
                                   GraphTextDiagnostics& diagnostics) noexcept {
    if (!value.IsObject()) {
        return Fail(diagnostics, GraphTextCode::InvalidValue, value,
                    "record value must be an object");
    }

    for (usize index = 0; index < value.Size(); ++index) {
        const StringView key = value.KeyAt(index);
        if (!(key == "record") && !(key == "type") && !(key == "value")) {
            return Fail(diagnostics, GraphTextCode::UnknownKey, value.ValueAt(index),
                        "unknown key in record value");
        }
    }

    const JsonValue bytes = value.Find("record");
    const JsonValue type = value.Find("type");
    const JsonValue fields = value.Find("value");
    if (bytes.IsValid()) {
        if (type.IsValid() || fields.IsValid()) {
            return Fail(diagnostics, GraphTextCode::InvalidValue, value,
                        "record value has both record and type");
        }

        return ReadRecordBytes(bytes, record, diagnostics);
    }

    if (!type.IsValid() || !fields.IsValid()) {
        return Fail(diagnostics, GraphTextCode::MissingKey, value,
                    "record value needs record or type and value");
    }

    if (!type.IsString() || type.GetString().IsEmpty() || !fields.IsObject()) {
        return Fail(diagnostics, GraphTextCode::InvalidValue, value,
                    "record type must be a non-empty string and value an object");
    }

    ObjectRegistry& objects = context.objects;
    auto lease = AcquireTypeByText(objects.Types(), type.GetString());
    if (!lease) {
        if (lease.Error() == ErrorCode::NotFound) {
            if (auto assigned = unresolvedType.TryAssign(type.GetString()); !assigned) {
                return Result<RecordRead>::Failure(assigned.Error());
            }

            record.bytes.Clear();
            return Result<RecordRead>::Success(RecordRead::Unresolved);
        }

        return Fail(diagnostics, GraphTextCode::InvalidValue, type, "record type is not a struct");
    }

    Array<uint8> saved(context.allocator);
    reflection_text::TextDiagnostics textDiagnostics(context.allocator);
    if (auto imported =
            ImportRecordText(context, lease.Value(), fields, use, saved, textDiagnostics);
        !imported) {
        if (imported.Error() == ErrorCode::OutOfMemory) {
            return Result<RecordRead>::Failure(imported.Error());
        }

        if (imported.Error() == ErrorCode::InvalidArgument && textDiagnostics.Count() != 0) {
            auto reported = ReportTextDiagnostics(context, textDiagnostics, fields, diagnostics);
            return Result<RecordRead>::Failure(reported.Error());
        }

        return Fail(diagnostics, GraphTextCode::RecordText, fields,
                    "record value cannot be stored as a standalone record");
    }

    if (saved.Size() > MaxPropertyRecordBytes) {
        return Fail(diagnostics, GraphTextCode::InvalidValue, fields, "record value is too large");
    }

    // 결과는 문서 쪽 allocator의 배열에 담는다(작업 allocator 버퍼를 문서로 넘기지 않는다).
    record.bytes.Clear();
    if (auto copied = record.bytes.TryAppend(saved.View()); !copied) {
        return Result<RecordRead>::Failure(copied.Error());
    }

    return Result<RecordRead>::Success(RecordRead::Resolved);
}

} // namespace rocks::nodegraph::text_detail
