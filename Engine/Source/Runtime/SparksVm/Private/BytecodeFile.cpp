#include <SparksVm/BytecodeFile.h>

#include <Core/Encoding/ByteReader.h>
#include <Core/Encoding/ByteWriter.h>

// `.sparkc` 쓰기·읽기와 verifier다(Docs/Design/SparksBytecode.ko.md 5절, LC4c).

namespace rocks::sparks {

namespace {

constexpr uint8 Magic[] = {'S', 'P', 'K', 'C'};
constexpr usize TagBytes = 4;
constexpr uint8 ConstantsTag[TagBytes] = {'K', 'C', 'O', 'N'};
constexpr uint8 ReferencesTag[TagBytes] = {'R', 'E', 'F', 'S'};
constexpr uint8 TypesTag[TagBytes] = {'T', 'Y', 'P', 'E'};
constexpr uint8 FunctionsTag[TagBytes] = {'F', 'U', 'N', 'C'};
constexpr uint8 ClassesTag[TagBytes] = {'C', 'L', 'A', 'S'};
constexpr uint8 DebugTag[TagBytes] = {'D', 'B', 'U', 'G'};

// 표 원소의 최소 byte 수다(개수를 믿기 전에 남은 byte와 비교한다 — 큰 할당 막기).
constexpr usize ScalarBytes = 8;
constexpr usize StringHeaderBytes = 4;
constexpr usize RefBytes = 13;
constexpr usize TypeBytes = 4 + 1 + 4 + 4 + 2 * BytecodeBankCount;
constexpr usize TypeFieldBytes = 4 + 1 + 1 + 4;
constexpr usize ClassBytes = 4 * 11 + 2 * BytecodeBankCount + 4;
constexpr usize HookBytes = 1 + 4;
constexpr usize HandlerBytes = 4 + 4 + 4 + 1 + 1 + 4 + 4 + 4;
constexpr usize HandlerPathBytes = 4;
constexpr usize EventBytes = 4 + 4;
constexpr usize FunctionBytes = 4 * 3 + 2 * 3 * BytecodeBankCount + 2 + BytecodeHashBytes + 4 * 4;
constexpr usize WordBytes = 4;
constexpr usize SwitchBytes = 12;
constexpr usize CaseBytes = 12;
constexpr usize AwaitBytes = 12;
constexpr usize LiveRangeBytes = 5;
constexpr usize LocationBytes = 8;

// profile 값의 상한(0 core, 1 full)이다.
constexpr uint8 MaxProfile = 1;

// 표시 종류의 마지막 값(Mat4)이다.
constexpr uint32 MaxDisplayKind = static_cast<uint32>(DisplayKind::Mat4);
// 컨테이너 표지의 slot 수 상한(값 하나의 slot 상한 18)이다.
constexpr uint32 MaxMarkerSlots = 18;

bool Fits(usize count, usize elementBytes, usize remaining) noexcept {
    return elementBytes == 0 || count <= remaining / elementBytes;
}

void WriteSection(ByteWriter& writer, const uint8 (&tag)[TagBytes], auto&& body) noexcept {
    writer.WriteBytes(Span<const uint8>(tag, TagBytes));
    const usize lengthAt = writer.Size();
    writer.WriteU32LE(0);
    const usize start = writer.Size();
    body();
    const usize length = writer.Size() - start;
    writer.PatchLittleEndian<uint32>(lengthAt, static_cast<uint32>(length));
}

bool IsTerminatorOpcode(Opcode op) noexcept {
    return op == Opcode::Jmp || op == Opcode::Ret || op == Opcode::RetErrS ||
           op == Opcode::RetErrB || op == Opcode::EndExecS || op == Opcode::EndExecB ||
           op == Opcode::Fault;
}

} // namespace

const char* BytecodeLoadErrorName(BytecodeLoadError error) noexcept {
    switch (error) {
    case BytecodeLoadError::None:
        return "none";
    case BytecodeLoadError::Truncated:
        return "truncated";
    case BytecodeLoadError::BadMagic:
        return "bad-magic";
    case BytecodeLoadError::UnsupportedVersion:
        return "unsupported-version";
    case BytecodeLoadError::BadSection:
        return "bad-section";
    case BytecodeLoadError::BadTable:
        return "bad-table";
    case BytecodeLoadError::BadFunction:
        return "bad-function";
    case BytecodeLoadError::BadOpcode:
        return "bad-opcode";
    case BytecodeLoadError::BadRegister:
        return "bad-register";
    case BytecodeLoadError::BadConstant:
        return "bad-constant";
    case BytecodeLoadError::BadReference:
        return "bad-reference";
    case BytecodeLoadError::BadBranch:
        return "bad-branch";
    case BytecodeLoadError::BadOperand:
        return "bad-operand";
    case BytecodeLoadError::BadEnd:
        return "bad-end";
    }

    return "?";
}

// ---- 쓰기 ----

Result<void> WriteBytecodeFile(const BytecodeModule& module, Array<uint8>& out) noexcept {
    if (module.words.Size() > NumericLimits<uint32>::Max() ||
        module.stringBytes.Size() > NumericLimits<uint32>::Max()) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    ByteWriter writer(out);
    writer.WriteBytes(Span<const uint8>(Magic, sizeof(Magic)));
    writer.WriteU32LE(BytecodeFormatVersion);
    writer.WriteU32LE(module.compiler);
    writer.WriteU32LE(module.path);
    writer.WriteBytes(Span<const uint8>(module.contentHash, BytecodeHashBytes));
    writer.WriteBytes(Span<const uint8>(module.interfaceHash, BytecodeHashBytes));
    writer.WriteU8(module.profile);
    WriteSection(writer, ConstantsTag, [&]() noexcept {
        writer.WriteU32LE(static_cast<uint32>(module.scalars.Size()));
        for (const uint64 value : module.scalars) {
            writer.WriteU64LE(value);
        }

        writer.WriteU32LE(static_cast<uint32>(module.stringOffsets.Size()));
        for (usize index = 0; index < module.stringOffsets.Size(); ++index) {
            const StringView text = module.String(static_cast<uint32>(index));
            writer.WriteU32LE(static_cast<uint32>(text.ByteCount()));
            writer.WriteBytes(
                Span<const uint8>(reinterpret_cast<const uint8*>(text.Data()), text.ByteCount()));
        }
    });
    WriteSection(writer, ReferencesTag, [&]() noexcept {
        writer.WriteU32LE(static_cast<uint32>(module.refs.Size()));
        for (const BytecodeRef& ref : module.refs) {
            writer.WriteU8(static_cast<uint8>(ref.kind));
            writer.WriteU32LE(ref.name);
            writer.WriteU32LE(ref.detail);
            writer.WriteU32LE(ref.arguments);
        }
    });
    WriteSection(writer, TypesTag, [&]() noexcept {
        writer.WriteU32LE(static_cast<uint32>(module.types.Size()));
        for (const BytecodeType& type : module.types) {
            writer.WriteU32LE(type.name);
            writer.WriteU8(static_cast<uint8>(type.kind));
            writer.WriteU32LE(type.firstField);
            writer.WriteU32LE(type.fieldCount);
            for (const uint16 slots : type.slots) {
                writer.WriteU16LE(slots);
            }
        }

        writer.WriteU32LE(static_cast<uint32>(module.typeFields.Size()));
        for (const BytecodeTypeField& field : module.typeFields) {
            writer.WriteU32LE(field.name);
            writer.WriteU8(field.bank);
            writer.WriteU8(field.slots);
            writer.WriteU32LE(field.offset);
        }
    });
    WriteSection(writer, ClassesTag, [&]() noexcept {
        writer.WriteU32LE(static_cast<uint32>(module.classes.Size()));
        for (const BytecodeClass& entry : module.classes) {
            const uint32 words[] = {entry.name,       entry.baseClass,    entry.baseName,
                                    entry.firstField, entry.fieldCount,   entry.firstHook,
                                    entry.hookCount,  entry.firstHandler, entry.handlerCount,
                                    entry.firstEvent, entry.eventCount};
            for (const uint32 word : words) {
                writer.WriteU32LE(word);
            }

            for (const uint16 slots : entry.slots) {
                writer.WriteU16LE(slots);
            }

            writer.WriteU32LE(entry.initializer);
        }

        writer.WriteU32LE(static_cast<uint32>(module.classFields.Size()));
        for (const BytecodeTypeField& field : module.classFields) {
            writer.WriteU32LE(field.name);
            writer.WriteU8(field.bank);
            writer.WriteU8(field.slots);
            writer.WriteU32LE(field.offset);
        }

        writer.WriteU32LE(static_cast<uint32>(module.hooks.Size()));
        for (const BytecodeHook& hook : module.hooks) {
            writer.WriteU8(static_cast<uint8>(hook.kind));
            writer.WriteU32LE(hook.function);
        }

        writer.WriteU32LE(static_cast<uint32>(module.handlers.Size()));
        for (const BytecodeHandler& handler : module.handlers) {
            writer.WriteU32LE(handler.path);
            writer.WriteU32LE(handler.event);
            writer.WriteU32LE(handler.function);
            writer.WriteU8(static_cast<uint8>(handler.reentry));
            writer.WriteU8(handler.awaits);
            writer.WriteU32LE(handler.firstPath);
            writer.WriteU32LE(handler.pathCount);
            writer.WriteU32LE(handler.eventRef);
        }

        writer.WriteU32LE(static_cast<uint32>(module.handlerPaths.Size()));
        for (const uint32 ref : module.handlerPaths) {
            writer.WriteU32LE(ref);
        }

        writer.WriteU32LE(static_cast<uint32>(module.events.Size()));
        for (const BytecodeEvent& event : module.events) {
            writer.WriteU32LE(event.name);
            writer.WriteU32LE(event.arguments);
        }
    });
    WriteSection(writer, FunctionsTag, [&]() noexcept {
        writer.WriteU32LE(static_cast<uint32>(module.functions.Size()));
        for (const BytecodeFunction& function : module.functions) {
            writer.WriteU32LE(function.name);
            writer.WriteU32LE(function.firstWord);
            writer.WriteU32LE(function.wordCount);
            for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
                writer.WriteU16LE(function.registers[bank]);
                writer.WriteU16LE(function.frame[bank]);
                writer.WriteU16LE(function.parameters[bank]);
            }

            writer.WriteU8(function.returnBank);
            writer.WriteU8(function.returnSlots);
            writer.WriteBytes(Span<const uint8>(function.bodyHash, BytecodeHashBytes));

            writer.WriteU32LE(function.firstSwitch);
            writer.WriteU32LE(function.switchCount);
            writer.WriteU32LE(function.firstAwait);
            writer.WriteU32LE(function.awaitCount);
        }

        writer.WriteU32LE(static_cast<uint32>(module.words.Size()));
        for (const uint32 word : module.words) {
            writer.WriteU32LE(word);
        }

        writer.WriteU32LE(static_cast<uint32>(module.switches.Size()));
        for (const BytecodeSwitchTable& table : module.switches) {
            writer.WriteU32LE(table.firstCase);
            writer.WriteU32LE(table.caseCount);
            writer.WriteU32LE(table.defaultTarget);
        }

        writer.WriteU32LE(static_cast<uint32>(module.cases.Size()));
        for (const BytecodeSwitchCase& entry : module.cases) {
            writer.WriteU64LE(static_cast<uint64>(entry.value));
            writer.WriteU32LE(entry.target);
        }

        writer.WriteU32LE(static_cast<uint32>(module.awaits.Size()));
        for (const BytecodeAwaitPoint& point : module.awaits) {
            writer.WriteU32LE(point.address);
            writer.WriteU32LE(point.firstLive);
            writer.WriteU32LE(point.liveCount);
        }

        writer.WriteU32LE(static_cast<uint32>(module.liveRanges.Size()));
        for (const BytecodeLiveRange& range : module.liveRanges) {
            writer.WriteU8(static_cast<uint8>(range.bank));
            writer.WriteU16LE(range.first);
            writer.WriteU16LE(range.count);
        }
    });
    WriteSection(writer, DebugTag, [&]() noexcept {
        writer.WriteU32LE(static_cast<uint32>(module.locations.Size()));
        for (const BytecodeLocation& at : module.locations) {
            writer.WriteU32LE(at.line);
            writer.WriteU32LE(at.column);
        }
    });
    return writer.Status();
}

// ---- 읽기 ----

namespace {

class FileReader final {
public:
    FileReader(Span<const uint8> bytes, BytecodeModule& out, BytecodeRejection& rejection) noexcept
        : reader_(bytes), out_(out), rejection_(rejection) {
    }

    Result<void> Read() noexcept {
        const Span<const uint8> magic = reader_.Take(sizeof(Magic));
        if (reader_.Failed()) {
            return Reject(BytecodeLoadError::Truncated);
        }

        for (usize index = 0; index < sizeof(Magic); ++index) {
            if (magic[index] != Magic[index]) {
                return Reject(BytecodeLoadError::BadMagic);
            }
        }

        const uint32 version = reader_.ReadU32LE();
        if (reader_.Failed()) {
            return Reject(BytecodeLoadError::Truncated);
        }

        if (version != BytecodeFormatVersion) {
            return Reject(BytecodeLoadError::UnsupportedVersion);
        }

        out_.compiler = reader_.ReadU32LE();
        out_.path = reader_.ReadU32LE();
        static_cast<void>(reader_.ReadInto(Span<uint8>(out_.contentHash, BytecodeHashBytes)));
        static_cast<void>(reader_.ReadInto(Span<uint8>(out_.interfaceHash, BytecodeHashBytes)));
        out_.profile = reader_.ReadU8();
        if (reader_.Failed()) {
            return Reject(BytecodeLoadError::Truncated);
        }

        if (out_.profile > MaxProfile) {
            return Reject(BytecodeLoadError::BadTable);
        }

        if (auto read = Section(ConstantsTag, &FileReader::Constants); !read) {
            return read;
        }

        if (auto read = Section(ReferencesTag, &FileReader::References); !read) {
            return read;
        }

        if (auto read = Section(TypesTag, &FileReader::Types); !read) {
            return read;
        }

        if (auto read = Section(ClassesTag, &FileReader::Classes); !read) {
            return read;
        }

        if (auto read = Section(FunctionsTag, &FileReader::Functions); !read) {
            return read;
        }

        if (auto read = Section(DebugTag, &FileReader::Debug); !read) {
            return read;
        }

        if (reader_.Remaining() != 0) {
            return Reject(BytecodeLoadError::BadSection);
        }

        rejection_ = VerifyBytecode(out_);
        return rejection_.error == BytecodeLoadError::None
                   ? Result<void>::Success()
                   : Result<void>::Failure(ErrorCode::InvalidArgument);
    }

private:
    Result<void> Reject(BytecodeLoadError error) noexcept {
        rejection_ = BytecodeRejection{error, NoBytecodeIndex, NoBytecodeIndex};
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Result<void> Section(const uint8 (&tag)[TagBytes],
                         Result<void> (FileReader::*body)(ByteReader&) noexcept) noexcept {
        const Span<const uint8> actual = reader_.Take(TagBytes);
        const uint32 length = reader_.ReadU32LE();
        if (reader_.Failed()) {
            return Reject(BytecodeLoadError::Truncated);
        }

        for (usize index = 0; index < TagBytes; ++index) {
            if (actual[index] != tag[index]) {
                return Reject(BytecodeLoadError::BadSection);
            }
        }

        if (length > reader_.Remaining()) {
            return Reject(BytecodeLoadError::Truncated);
        }

        ByteReader section = reader_.Sub(length);
        if (auto read = (this->*body)(section); !read) {
            return read;
        }

        if (section.Failed() || section.Remaining() != 0) {
            return Reject(BytecodeLoadError::BadSection);
        }

        return Result<void>::Success();
    }

    // 개수를 읽고 남은 byte가 그만큼 있는지 본다(없으면 잘린 구역).
    bool Count(ByteReader& reader, usize elementBytes, uint32& count) noexcept {
        count = reader.ReadU32LE();
        return !reader.Failed() && Fits(count, elementBytes, reader.Remaining());
    }

    template <typename T>
    bool Resize(Array<T>& array, usize count) noexcept {
        if (auto resized = array.TryResize(count); !resized) {
            error_ = resized.Error();
            return false;
        }

        return true;
    }

    Result<void> Failure() noexcept {
        return error_ != ErrorCode::None ? Result<void>::Failure(error_)
                                         : Reject(BytecodeLoadError::BadSection);
    }

    Result<void> Constants(ByteReader& reader) noexcept {
        uint32 count = 0;
        if (!Count(reader, ScalarBytes, count) || !Resize(out_.scalars, count)) {
            return Failure();
        }

        for (uint64& value : out_.scalars) {
            value = reader.ReadU64LE();
        }

        if (!Count(reader, StringHeaderBytes, count) || !Resize(out_.stringOffsets, count) ||
            !Resize(out_.stringLengths, count)) {
            return Failure();
        }

        for (uint32 index = 0; index < count; ++index) {
            const uint32 length = reader.ReadU32LE();
            const Span<const uint8> bytes = reader.Take(length);
            if (reader.Failed()) {
                return Reject(BytecodeLoadError::BadSection); // 구역 안의 불일치
            }

            out_.stringOffsets[index] = static_cast<uint32>(out_.stringBytes.Size());
            out_.stringLengths[index] = length;
            if (auto appended = out_.stringBytes.TryAppend(
                    Span<const char>(reinterpret_cast<const char*>(bytes.Data()), bytes.Size()));
                !appended) {
                return appended;
            }
        }

        return Result<void>::Success();
    }

    Result<void> References(ByteReader& reader) noexcept {
        uint32 count = 0;
        if (!Count(reader, RefBytes, count) || !Resize(out_.refs, count)) {
            return Failure();
        }

        for (BytecodeRef& ref : out_.refs) {
            const uint8 kind = reader.ReadU8();
            if (kind > static_cast<uint8>(BytecodeRefKind::Literal)) {
                return Reject(BytecodeLoadError::BadTable);
            }

            ref.kind = static_cast<BytecodeRefKind>(kind);
            ref.name = reader.ReadU32LE();
            ref.detail = reader.ReadU32LE();
            ref.arguments = reader.ReadU32LE();
        }

        return Result<void>::Success();
    }

    Result<void> Types(ByteReader& reader) noexcept {
        uint32 count = 0;
        if (!Count(reader, TypeBytes, count) || !Resize(out_.types, count)) {
            return Failure();
        }

        for (BytecodeType& type : out_.types) {
            type.name = reader.ReadU32LE();
            const uint8 kind = reader.ReadU8();
            if (kind > static_cast<uint8>(BytecodeTypeKind::Enum)) {
                return Reject(BytecodeLoadError::BadTable);
            }

            type.kind = static_cast<BytecodeTypeKind>(kind);
            type.firstField = reader.ReadU32LE();
            type.fieldCount = reader.ReadU32LE();
            for (uint16& slots : type.slots) {
                slots = reader.ReadU16LE();
            }
        }

        if (!Count(reader, TypeFieldBytes, count) || !Resize(out_.typeFields, count)) {
            return Failure();
        }

        for (BytecodeTypeField& field : out_.typeFields) {
            field.name = reader.ReadU32LE();
            field.bank = reader.ReadU8();
            field.slots = reader.ReadU8();
            field.offset = reader.ReadU32LE();
        }

        return Result<void>::Success();
    }

    Result<void> Classes(ByteReader& reader) noexcept {
        uint32 count = 0;
        if (!Count(reader, ClassBytes, count) || !Resize(out_.classes, count)) {
            return Failure();
        }

        for (BytecodeClass& entry : out_.classes) {
            uint32* words[] = {&entry.name,       &entry.baseClass,    &entry.baseName,
                               &entry.firstField, &entry.fieldCount,   &entry.firstHook,
                               &entry.hookCount,  &entry.firstHandler, &entry.handlerCount,
                               &entry.firstEvent, &entry.eventCount};
            for (uint32* word : words) {
                *word = reader.ReadU32LE();
            }

            for (uint16& slots : entry.slots) {
                slots = reader.ReadU16LE();
            }

            entry.initializer = reader.ReadU32LE();
        }

        if (!Count(reader, TypeFieldBytes, count) || !Resize(out_.classFields, count)) {
            return Failure();
        }

        for (BytecodeTypeField& field : out_.classFields) {
            field.name = reader.ReadU32LE();
            field.bank = reader.ReadU8();
            field.slots = reader.ReadU8();
            field.offset = reader.ReadU32LE();
        }

        if (!Count(reader, HookBytes, count) || !Resize(out_.hooks, count)) {
            return Failure();
        }

        for (BytecodeHook& hook : out_.hooks) {
            const uint8 kind = reader.ReadU8();
            if (kind >= static_cast<uint8>(BytecodeHookKind::Count)) {
                return Reject(BytecodeLoadError::BadTable);
            }

            hook.kind = static_cast<BytecodeHookKind>(kind);
            hook.function = reader.ReadU32LE();
        }

        if (!Count(reader, HandlerBytes, count) || !Resize(out_.handlers, count)) {
            return Failure();
        }

        for (BytecodeHandler& handler : out_.handlers) {
            handler.path = reader.ReadU32LE();
            handler.event = reader.ReadU32LE();
            handler.function = reader.ReadU32LE();
            const uint8 reentry = reader.ReadU8();
            if (reentry > static_cast<uint8>(BytecodeReentry::Restart)) {
                return Reject(BytecodeLoadError::BadTable);
            }

            handler.reentry = static_cast<BytecodeReentry>(reentry);
            handler.awaits = reader.ReadU8();
            handler.firstPath = reader.ReadU32LE();
            handler.pathCount = reader.ReadU32LE();
            handler.eventRef = reader.ReadU32LE();
        }

        if (!Count(reader, HandlerPathBytes, count) || !Resize(out_.handlerPaths, count)) {
            return Failure();
        }

        for (uint32& ref : out_.handlerPaths) {
            ref = reader.ReadU32LE();
        }

        if (!Count(reader, EventBytes, count) || !Resize(out_.events, count)) {
            return Failure();
        }

        for (BytecodeEvent& event : out_.events) {
            event.name = reader.ReadU32LE();
            event.arguments = reader.ReadU32LE();
        }

        return Result<void>::Success();
    }

    Result<void> Functions(ByteReader& reader) noexcept {
        uint32 count = 0;
        if (!Count(reader, FunctionBytes, count) || !Resize(out_.functions, count)) {
            return Failure();
        }

        for (BytecodeFunction& function : out_.functions) {
            function.name = reader.ReadU32LE();
            function.firstWord = reader.ReadU32LE();
            function.wordCount = reader.ReadU32LE();
            for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
                function.registers[bank] = reader.ReadU16LE();
                function.frame[bank] = reader.ReadU16LE();
                function.parameters[bank] = reader.ReadU16LE();
            }

            function.returnBank = reader.ReadU8();
            function.returnSlots = reader.ReadU8();
            static_cast<void>(reader.ReadInto(Span<uint8>(function.bodyHash, BytecodeHashBytes)));

            function.firstSwitch = reader.ReadU32LE();
            function.switchCount = reader.ReadU32LE();
            function.firstAwait = reader.ReadU32LE();
            function.awaitCount = reader.ReadU32LE();
        }

        if (!Count(reader, WordBytes, count) || !Resize(out_.words, count)) {
            return Failure();
        }

        for (uint32& word : out_.words) {
            word = reader.ReadU32LE();
        }

        if (!Count(reader, SwitchBytes, count) || !Resize(out_.switches, count)) {
            return Failure();
        }

        for (BytecodeSwitchTable& table : out_.switches) {
            table.firstCase = reader.ReadU32LE();
            table.caseCount = reader.ReadU32LE();
            table.defaultTarget = reader.ReadU32LE();
        }

        if (!Count(reader, CaseBytes, count) || !Resize(out_.cases, count)) {
            return Failure();
        }

        for (BytecodeSwitchCase& entry : out_.cases) {
            entry.value = static_cast<int64>(reader.ReadU64LE());
            entry.target = reader.ReadU32LE();
        }

        if (!Count(reader, AwaitBytes, count) || !Resize(out_.awaits, count)) {
            return Failure();
        }

        for (BytecodeAwaitPoint& point : out_.awaits) {
            point.address = reader.ReadU32LE();
            point.firstLive = reader.ReadU32LE();
            point.liveCount = reader.ReadU32LE();
        }

        if (!Count(reader, LiveRangeBytes, count) || !Resize(out_.liveRanges, count)) {
            return Failure();
        }

        for (BytecodeLiveRange& range : out_.liveRanges) {
            const uint8 bank = reader.ReadU8();
            if (bank >= BytecodeBankCount) {
                return Reject(BytecodeLoadError::BadTable);
            }

            range.bank = static_cast<BytecodeBank>(bank);
            range.first = reader.ReadU16LE();
            range.count = reader.ReadU16LE();
        }

        return Result<void>::Success();
    }

    Result<void> Debug(ByteReader& reader) noexcept {
        uint32 count = 0;
        if (!Count(reader, LocationBytes, count) || !Resize(out_.locations, count)) {
            return Failure();
        }

        for (BytecodeLocation& at : out_.locations) {
            at.line = reader.ReadU32LE();
            at.column = reader.ReadU32LE();
        }

        return Result<void>::Success();
    }

    ByteReader reader_;
    BytecodeModule& out_;
    BytecodeRejection& rejection_;
    ErrorCode error_ = ErrorCode::None;
};

// ---- verifier ----

class Verifier final {
public:
    explicit Verifier(const BytecodeModule& module) noexcept : module_(module) {
    }

    BytecodeRejection Run() noexcept {
        const usize strings = module_.stringOffsets.Size();
        if (module_.stringLengths.Size() != strings ||
            (module_.compiler != NoBytecodeIndex && module_.compiler >= strings) ||
            (module_.path != NoBytecodeIndex && module_.path >= strings) ||
            module_.locations.Size() != module_.words.Size()) {
            return Reject(BytecodeLoadError::BadTable);
        }

        for (usize index = 0; index < strings; ++index) {
            if (usize(module_.stringOffsets[index]) + module_.stringLengths[index] >
                module_.stringBytes.Size()) {
                return Reject(BytecodeLoadError::BadTable);
            }
        }

        for (const BytecodeRef& ref : module_.refs) {
            // 보조 번호: 리터럴은 글자, 같은 모듈 함수는 함수 표, Type은 TYPE 표, StructField는
            // TYPE 필드 표(Sparks struct — 형식 버전 2), 그 밖(타입 인자를 단 호출)은 Type 참조다.
            const bool detailOk =
                ref.kind == BytecodeRefKind::StructField
                    ? ref.detail == NoBytecodeIndex || ref.detail < module_.typeFields.Size()
                    : ref.detail == NoBytecodeIndex ||
                          (ref.kind == BytecodeRefKind::Literal ? ref.detail < strings
                           : ref.kind == BytecodeRefKind::Function
                               ? ref.detail < module_.functions.Size()
                           : ref.kind == BytecodeRefKind::Type
                               ? ref.detail < module_.types.Size()
                               : ref.detail < module_.refs.Size() &&
                                     module_.refs[ref.detail].kind == BytecodeRefKind::Type);
            // 리터럴 인자 표·언어 함수·native·import·가상·필드 서술자(SparksVm 6a·6.1·6.5절)는
            // 문자열이다.
            const bool argumentsOk =
                ref.arguments == NoBytecodeIndex ||
                ((ref.kind == BytecodeRefKind::Literal ||
                  ref.kind == BytecodeRefKind::LanguageFunction ||
                  ref.kind == BytecodeRefKind::NativeFunction ||
                  ref.kind == BytecodeRefKind::ImportedFunction ||
                  ref.kind == BytecodeRefKind::VirtualFunction ||
                  ref.kind == BytecodeRefKind::Event || ref.kind == BytecodeRefKind::Field ||
                  ref.kind == BytecodeRefKind::StructField) &&
                 ref.arguments < strings);
            // 같은 모듈 함수 참조는 함수 표 번호가 있어야 한다(CALL이 그 함수로 간다).
            const bool functionOk =
                ref.kind != BytecodeRefKind::Function || ref.detail != NoBytecodeIndex;
            if (ref.name >= strings || !detailOk || !argumentsOk || !functionOk) {
                return Reject(BytecodeLoadError::BadTable);
            }
        }

        // TYPE 표: 필드 구간, struct 필드는 자기 bank 묶음 안(Scalar·Ref·Buf 순 — 절대 slot 위치).
        for (const BytecodeType& type : module_.types) {
            if (type.name >= strings ||
                usize(type.firstField) + type.fieldCount > module_.typeFields.Size()) {
                return Reject(BytecodeLoadError::BadTable);
            }

            uint32 groupStart[BytecodeBankCount] = {};
            for (uint32 bank = 1; bank < BytecodeBankCount; ++bank) {
                groupStart[bank] = groupStart[bank - 1] + type.slots[bank - 1];
            }

            for (uint32 at = type.firstField; at < type.firstField + type.fieldCount; ++at) {
                const BytecodeTypeField& field = module_.typeFields[at];
                if (field.name >= strings) {
                    return Reject(BytecodeLoadError::BadTable);
                }

                if (type.kind == BytecodeTypeKind::Enum) {
                    continue;
                }

                if (field.bank >= BytecodeBankCount || field.slots == 0 ||
                    (field.bank != 0 && field.slots != 1) ||
                    field.offset < groupStart[field.bank] ||
                    usize(field.offset) + field.slots >
                        usize(groupStart[field.bank]) + type.slots[field.bank]) {
                    return Reject(BytecodeLoadError::BadTable);
                }
            }
        }

        for (const BytecodeSwitchTable& table : module_.switches) {
            if (usize(table.firstCase) + table.caseCount > module_.cases.Size()) {
                return Reject(BytecodeLoadError::BadTable);
            }
        }

        for (const BytecodeAwaitPoint& point : module_.awaits) {
            if (usize(point.firstLive) + point.liveCount > module_.liveRanges.Size()) {
                return Reject(BytecodeLoadError::BadTable);
            }
        }

        for (uint32 index = 0; index < module_.functions.Size(); ++index) {
            function_ = index;
            if (const BytecodeRejection rejection = Function(module_.functions[index]);
                rejection.error != BytecodeLoadError::None) {
                return rejection;
            }
        }

        // CLASS 표(형식 버전 3 — 함수 검사 뒤: 함수 표의 사유가 먼저다) — SparksRuntime 2.1절):
        // 범위, 기반은 앞선 클래스, 필드는 자기 bank 구역 안, hook 종류는 클래스 안에서 하나, 함수
        // 번호, 초기화 함수는 this(Ref 1) 하나.
        for (uint32 index = 0; index < module_.classes.Size(); ++index) {
            const BytecodeClass& entry = module_.classes[index];
            const bool ranges =
                usize(entry.firstField) + entry.fieldCount <= module_.classFields.Size() &&
                usize(entry.firstHook) + entry.hookCount <= module_.hooks.Size() &&
                usize(entry.firstHandler) + entry.handlerCount <= module_.handlers.Size() &&
                usize(entry.firstEvent) + entry.eventCount <= module_.events.Size();
            const bool base =
                (entry.baseClass == NoBytecodeIndex || entry.baseClass < index) &&
                (entry.baseName == NoBytecodeIndex || entry.baseName < strings) &&
                (entry.baseClass == NoBytecodeIndex || entry.baseName == NoBytecodeIndex);
            if (entry.name >= strings || !ranges || !base) {
                return Reject(BytecodeLoadError::BadTable);
            }

            if (entry.initializer != NoBytecodeIndex) {
                if (entry.initializer >= module_.functions.Size()) {
                    return Reject(BytecodeLoadError::BadTable);
                }

                const BytecodeFunction& init = module_.functions[entry.initializer];
                if (init.parameters[0] != 0 || init.parameters[1] != 1 || init.parameters[2] != 0 ||
                    init.returnSlots != 0) {
                    return Reject(BytecodeLoadError::BadTable);
                }
            }

            // 클래스 이름은 모듈 안에서 하나다(로컬 필드 묶음·기반 찾기가 첫 일치를 고르지 않게).
            for (uint32 before = 0; before < index; ++before) {
                if (module_.String(module_.classes[before].name) == module_.String(entry.name)) {
                    return Reject(BytecodeLoadError::BadTable);
                }
            }

            // 필드는 선언 순으로 bank마다 빈틈없이 놓이고, bank별 합이 클래스 slot 수다(LC6a 리뷰
            // S3 — 필드 없는 큰 slot 수로 instance 저장소를 부풀리지 못한다).
            uint32 running[BytecodeBankCount] = {};
            for (uint32 at = entry.firstField; at < entry.firstField + entry.fieldCount; ++at) {
                const BytecodeTypeField& field = module_.classFields[at];
                if (field.name >= strings || field.bank >= BytecodeBankCount || field.slots == 0 ||
                    (field.bank != 0 && field.slots != 1) || field.offset != running[field.bank]) {
                    return Reject(BytecodeLoadError::BadTable);
                }

                running[field.bank] += field.slots;
            }

            for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
                if (running[bank] != entry.slots[bank]) {
                    return Reject(BytecodeLoadError::BadTable);
                }
            }

            uint32 seen = 0;
            for (uint32 at = entry.firstHook; at < entry.firstHook + entry.hookCount; ++at) {
                const BytecodeHook& hook = module_.hooks[at];
                const uint32 bit = 1u << static_cast<uint32>(hook.kind);
                if (hook.function >= module_.functions.Size() || (seen & bit) != 0) {
                    return Reject(BytecodeLoadError::BadTable);
                }

                // hook 함수 모양(SL 5.1절): this(Ref) + 종류별 인자(최대), 반환 없음.
                const BytecodeFunction& function = module_.functions[hook.function];
                uint16 expected[BytecodeBankCount] = {0, 1, 0};
                switch (hook.kind) {
                case BytecodeHookKind::Tick:
                case BytecodeHookKind::FixedTick:
                case BytecodeHookKind::LateTick:
                    expected[0] = 1; // dt
                    break;
                case BytecodeHookKind::TriggerEnter:
                case BytecodeHookKind::TriggerExit:
                    expected[1] = 3; // other, trigger
                    break;
                case BytecodeHookKind::CollisionEnter:
                case BytecodeHookKind::CollisionExit:
                    expected[1] = 2; // other
                    expected[2] = 1; // hit(엔진 struct)
                    break;
                default:
                    break;
                }

                // 인자는 앞에서부터 필요한 만큼만 선언할 수 있다(this는 늘 있다).
                for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
                    if (function.parameters[bank] > expected[bank]) {
                        return Reject(BytecodeLoadError::BadTable);
                    }
                }

                if (function.parameters[1] == 0) {
                    return Reject(BytecodeLoadError::BadTable);
                }

                if (function.returnSlots != 0) {
                    return Reject(BytecodeLoadError::BadTable);
                }

                seen |= bit;
            }

            for (uint32 at = entry.firstHandler; at < entry.firstHandler + entry.handlerCount;
                 ++at) {
                const BytecodeHandler& handler = module_.handlers[at];
                if (handler.path >= strings || handler.event >= strings ||
                    handler.function >= module_.functions.Size() || handler.awaits > 1 ||
                    (handler.reentry != BytecodeReentry::None && handler.awaits == 0) ||
                    usize(handler.firstPath) + handler.pathCount > module_.handlerPaths.Size() ||
                    (handler.eventRef != NoBytecodeIndex &&
                     (handler.eventRef >= module_.refs.Size() ||
                      module_.refs[handler.eventRef].kind != BytecodeRefKind::Event))) {
                    return Reject(BytecodeLoadError::BadTable);
                }

                // 경로 마디는 필드·문맥 참조다.
                for (uint32 step = handler.firstPath; step < handler.firstPath + handler.pathCount;
                     ++step) {
                    const uint32 ref = module_.handlerPaths[step];
                    if (ref >= module_.refs.Size() ||
                        (module_.refs[ref].kind != BytecodeRefKind::Field &&
                         module_.refs[ref].kind != BytecodeRefKind::Context)) {
                        return Reject(BytecodeLoadError::BadTable);
                    }
                }
            }

            for (uint32 at = entry.firstEvent; at < entry.firstEvent + entry.eventCount; ++at) {
                const BytecodeEvent& event = module_.events[at];
                if (event.name >= strings ||
                    (event.arguments != NoBytecodeIndex && event.arguments >= strings)) {
                    return Reject(BytecodeLoadError::BadTable);
                }
            }
        }

        return BytecodeRejection{};
    }

private:
    BytecodeRejection Reject(BytecodeLoadError error, uint32 address = NoBytecodeIndex) noexcept {
        return BytecodeRejection{
            error, error == BytecodeLoadError::BadTable ? NoBytecodeIndex : function_, address};
    }

    bool Register(OperandBank bank, uint32 number, uint32 count = 1) const noexcept {
        if (bank == OperandBank::None) {
            return true;
        }

        const uint32 index = static_cast<uint32>(bank) - 1;
        return number + count <= frame_[index];
    }

    bool Ref(uint32 index, BytecodeRefKind kind) const noexcept {
        return index < module_.refs.Size() && module_.refs[index].kind == kind;
    }

    bool CallRef(Opcode op, uint32 index) const noexcept {
        if (index >= module_.refs.Size()) {
            return false;
        }

        const BytecodeRefKind kind = module_.refs[index].kind;
        switch (op) {
        case Opcode::Call:
            return kind == BytecodeRefKind::Function || kind == BytecodeRefKind::ImportedFunction;
        case Opcode::CallV:
            return kind == BytecodeRefKind::VirtualFunction;
        case Opcode::CallN:
            return kind == BytecodeRefKind::NativeFunction;
        case Opcode::CallL:
            return kind == BytecodeRefKind::LanguageFunction;
        case Opcode::CallRpc:
            return kind == BytecodeRefKind::RpcFunction;
        default:
            return kind == BytecodeRefKind::Event;
        }
    }

    bool Marker(uint32 marker) const noexcept {
        // Ref·Buf 묶음은 slot 하나다(값 하나 — VM은 그 자리를 하나의 handle·버퍼로 다룬다).
        const uint32 valueBank = (marker >> 5) & 0x3;
        const uint32 keyBank = (marker >> 13) & 0x3;
        const uint32 valueSlots = marker & 0x1F;
        const uint32 keySlots = (marker >> 8) & 0x1F;
        return valueBank < BytecodeBankCount && keyBank < BytecodeBankCount &&
               valueSlots <= MaxMarkerSlots && keySlots <= MaxMarkerSlots &&
               (valueBank == 0 || valueSlots <= 1) && (keyBank == 0 || keySlots <= 1) &&
               (marker >> 15) == 0;
    }

    BytecodeRejection Function(const BytecodeFunction& function) noexcept {
        if (function.name >= module_.stringOffsets.Size() ||
            usize(function.firstWord) + function.wordCount > module_.words.Size() ||
            function.wordCount == 0 || function.wordCount > MaxFunctionWords ||
            usize(function.firstSwitch) + function.switchCount > module_.switches.Size() ||
            usize(function.firstAwait) + function.awaitCount > module_.awaits.Size() ||
            function.returnBank >= BytecodeBankCount || function.returnSlots > MaxMarkerSlots) {
            return Reject(BytecodeLoadError::BadFunction);
        }

        for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
            if (function.parameters[bank] > function.registers[bank] ||
                function.registers[bank] > function.frame[bank] ||
                function.frame[bank] > MaxBankRegisters) {
                return Reject(BytecodeLoadError::BadFunction);
            }

            frame_[bank] = function.frame[bank];
        }

        // 반환 값은 frame 0번부터 돌려준다.
        if (function.returnSlots > function.frame[function.returnBank]) {
            return Reject(BytecodeLoadError::BadFunction);
        }

        current_ = &function;

        // 명령 시작 표(EXTRA 단어는 시작이 아니다).
        const uint32* words = module_.words.Data() + function.firstWord;
        for (uint64& bits : starts_) {
            bits = 0;
        }

        for (uint64& bits : awaited_) {
            bits = 0;
        }

        uint32 last = 0;
        for (uint32 address = 0; address < function.wordCount; ++address) {
            const uint32 op = words[address] & 0xFF;
            if (op >= static_cast<uint32>(Opcode::Count) ||
                static_cast<Opcode>(op) == Opcode::Extra) {
                return Reject(BytecodeLoadError::BadOpcode, address);
            }

            starts_[address / StartBits] |= uint64(1) << (address % StartBits);
            last = address;
            if (InfoOf(static_cast<Opcode>(op)).extra) {
                if (address + 1 >= function.wordCount ||
                    DecodeOpcode(words[address + 1]) != Opcode::Extra) {
                    return Reject(BytecodeLoadError::BadOpcode, address);
                }

                ++address;
            }
        }

        if (!IsTerminatorOpcode(DecodeOpcode(words[last]))) {
            return Reject(BytecodeLoadError::BadEnd, last);
        }

        auto target = [&](int64 address) noexcept {
            return address >= 0 && address < function.wordCount && IsStart(address);
        };

        // await 표: 주소마다 AWAIT·AWAITEV 시작이고 한 번만 나온다(생존 구간은 할당 수 안).
        for (uint32 index = function.firstAwait; index < function.firstAwait + function.awaitCount;
             ++index) {
            const BytecodeAwaitPoint& point = module_.awaits[index];
            if (!target(point.address) ||
                (DecodeOpcode(words[point.address]) != Opcode::Await &&
                 DecodeOpcode(words[point.address]) != Opcode::AwaitEv) ||
                IsAwaited(point.address)) {
                return Reject(BytecodeLoadError::BadBranch, point.address);
            }

            awaited_[point.address / StartBits] |= uint64(1) << (point.address % StartBits);
            for (uint32 at = point.firstLive; at < point.firstLive + point.liveCount; ++at) {
                const BytecodeLiveRange& range = module_.liveRanges[at];
                if (static_cast<uint32>(range.bank) >= BytecodeBankCount ||
                    range.first + range.count >
                        function.registers[static_cast<uint32>(range.bank)]) {
                    return Reject(BytecodeLoadError::BadRegister, point.address);
                }
            }
        }

        for (uint32 address = 0; address < function.wordCount; ++address) {
            const uint32 word = words[address];
            const OpcodeInfo& info = InfoOf(DecodeOpcode(word));
            const uint32 extra = info.extra ? words[address + 1] : 0;
            if (const BytecodeLoadError error = Instruction(info, word, extra, address, target);
                error != BytecodeLoadError::None) {
                return Reject(error, address);
            }

            // 정지 명령은 모두 await 표에 있어야 한다(정지 중 보존할 레지스터).
            if ((info.op == Opcode::Await || info.op == Opcode::AwaitEv) && !IsAwaited(address)) {
                return Reject(BytecodeLoadError::BadBranch, address);
            }

            address += info.extra ? 1 : 0;
        }

        for (uint32 index = function.firstSwitch;
             index < function.firstSwitch + function.switchCount; ++index) {
            const BytecodeSwitchTable& table = module_.switches[index];
            if (!target(table.defaultTarget)) {
                return Reject(BytecodeLoadError::BadBranch);
            }

            for (uint32 at = table.firstCase; at < table.firstCase + table.caseCount; ++at) {
                if (!target(module_.cases[at].target)) {
                    return Reject(BytecodeLoadError::BadBranch);
                }
            }
        }

        return BytecodeRejection{};
    }

    // 호출 창(frame의 할당 수 위)에 bank마다 count slot이 들어가는지다.
    bool Window(uint32 bank, uint32 count) const noexcept {
        return current_->registers[bank] + count <= current_->frame[bank];
    }

    // bank 번호(0–2)로 본 레지스터 묶음이 frame 안인지다. 묶음 slot 수가 0이면 거부한다.
    bool Tuple(uint32 bank, uint32 first, uint32 slots) const noexcept {
        return bank < BytecodeBankCount && slots >= 1 && slots <= MaxMarkerSlots &&
               first + slots <= frame_[bank];
    }

    // NEWLIT 인자 표(`이름=bank 글자;…`)의 항목 수와 bank별 수다. 형식이 틀리면 false.
    bool LiteralArguments(uint32 arguments, uint32 (&perBank)[BytecodeBankCount],
                          uint32& count) const noexcept {
        for (uint32& value : perBank) {
            value = 0;
        }

        count = 0;
        if (arguments == NoBytecodeIndex) {
            return true;
        }

        const StringView text = module_.String(arguments);
        usize at = 0;
        while (at < text.ByteCount()) {
            usize equals = at;
            while (equals < text.ByteCount() && text.Data()[equals] != '=') {
                ++equals;
            }

            if (equals == at || equals + 1 >= text.ByteCount()) {
                return false;
            }

            const char letter = text.Data()[equals + 1];
            const uint32 bank = letter == 's' ? 0 : letter == 'r' ? 1 : letter == 'b' ? 2 : 3;
            if (bank >= BytecodeBankCount) {
                return false;
            }

            ++perBank[bank];
            ++count;
            at = equals + 2;
            if (at < text.ByteCount()) {
                if (text.Data()[at] != ';') {
                    return false;
                }

                ++at;
            }
        }

        return true;
    }

    template <typename Target>
    BytecodeLoadError Instruction(const OpcodeInfo& info, uint32 word, uint32 extra, uint32 address,
                                  Target&& target) noexcept {
        const uint32 a = DecodeA(word);
        const uint32 b = DecodeB(word);
        const uint32 c = DecodeC(word);
        const uint32 bx = DecodeBx(word);
        const uint32 ax = DecodeAx(extra);
        const int64 next = static_cast<int64>(address) + 1;
        constexpr uint32 S = 0;
        constexpr uint32 R = 1;
        constexpr uint32 B = 2;
        auto bankOf = [](OperandBank bank) noexcept { return static_cast<uint32>(bank) - 1; };
        auto ok = [](bool condition, BytecodeLoadError error) noexcept {
            return condition ? BytecodeLoadError::None : error;
        };
        switch (info.op) {
        case Opcode::LoadK:
            return !Tuple(S, a, 1)                ? BytecodeLoadError::BadRegister
                   : bx >= module_.scalars.Size() ? BytecodeLoadError::BadConstant
                                                  : BytecodeLoadError::None;
        case Opcode::LoadStr:
            return !Tuple(B, a, 1)                      ? BytecodeLoadError::BadRegister
                   : bx >= module_.stringOffsets.Size() ? BytecodeLoadError::BadConstant
                                                        : BytecodeLoadError::None;
        case Opcode::Jmp:
            return a != 0                           ? BytecodeLoadError::BadOperand
                   : target(next + DecodeSBx(word)) ? BytecodeLoadError::None
                                                    : BytecodeLoadError::BadBranch;
        case Opcode::Brt:
        case Opcode::Brf:
            return !Tuple(S, a, 1)                   ? BytecodeLoadError::BadRegister
                   : !target(next + DecodeSBx(word)) ? BytecodeLoadError::BadBranch
                                                     : BytecodeLoadError::None;
        case Opcode::Switch:
            return !Tuple(S, a, 1) ? BytecodeLoadError::BadRegister
                   : bx < current_->firstSwitch ||
                           bx >= current_->firstSwitch + current_->switchCount
                       ? BytecodeLoadError::BadBranch
                       : BytecodeLoadError::None;
        case Opcode::Ret:
            // 반환 모양은 함수 표와 같다(값 없음은 A·B·C 모두 0).
            if (b == 0) {
                return ok(a == 0 && c == 0 && current_->returnSlots == 0,
                          BytecodeLoadError::BadOperand);
            }

            return b != current_->returnSlots || c != current_->returnBank
                       ? BytecodeLoadError::BadOperand
                       : ok(Tuple(c, a, b), BytecodeLoadError::BadRegister);
        case Opcode::RetErrS:
            return current_->returnSlots == 0 || b == 0
                       ? BytecodeLoadError::BadOperand
                       : ok(Tuple(S, a, b), BytecodeLoadError::BadRegister);
        case Opcode::RetErrB:
            return current_->returnSlots == 0 ? BytecodeLoadError::BadOperand
                                              : ok(Tuple(B, a, 1), BytecodeLoadError::BadRegister);
        case Opcode::EndExecS:
            return b == 0 ? BytecodeLoadError::BadOperand
                          : ok(Tuple(S, a, b), BytecodeLoadError::BadRegister);
        case Opcode::EndExecB:
            return ok(Tuple(B, a, 1), BytecodeLoadError::BadRegister);
        case Opcode::Conv: {
            // 숫자 타입 사이의 변환만이다(I8…F64).
            const uint32 from = c >> 4;
            const uint32 to = c & 0xF;
            const bool numeric = from >= static_cast<uint32>(ScalarKind::I8) &&
                                 from <= static_cast<uint32>(ScalarKind::F64) &&
                                 to >= static_cast<uint32>(ScalarKind::I8) &&
                                 to <= static_cast<uint32>(ScalarKind::F64);
            return !numeric ? BytecodeLoadError::BadOperand
                            : ok(Tuple(S, a, 1) && Tuple(S, b, 1), BytecodeLoadError::BadRegister);
        }
        case Opcode::GetCtx:
            return !Tuple(R, a, 1)                      ? BytecodeLoadError::BadRegister
                   : !Ref(bx, BytecodeRefKind::Context) ? BytecodeLoadError::BadReference
                                                        : BytecodeLoadError::None;
        case Opcode::NewS:
            // TYPE이 있으면 struct여야 한다(배치로 0·null 버퍼를 만든다). 엔진 struct·TagSet은
            // TYPE이 없다(host 배치 — LC5d, VM은 그때까지 ContractViolation).
            return !Tuple(B, a, 1)                   ? BytecodeLoadError::BadRegister
                   : !Ref(bx, BytecodeRefKind::Type) ? BytecodeLoadError::BadReference
                   : module_.refs[bx].detail != NoBytecodeIndex &&
                           module_.types[module_.refs[bx].detail].kind != BytecodeTypeKind::Struct
                       ? BytecodeLoadError::BadReference
                       : BytecodeLoadError::None;
        case Opcode::NewC:
        case Opcode::NewA:
        case Opcode::NewSet:
        case Opcode::NewM:
            return !Tuple(bankOf(info.a), a, 1)      ? BytecodeLoadError::BadRegister
                   : !Ref(bx, BytecodeRefKind::Type) ? BytecodeLoadError::BadReference
                                                     : BytecodeLoadError::None;
        case Opcode::Call:
        case Opcode::CallV:
        case Opcode::CallN:
        case Opcode::CallL:
        case Opcode::CallRpc:
        case Opcode::Emit: {
            if (a != 0) {
                return BytecodeLoadError::BadOperand;
            }

            if (!CallRef(info.op, bx)) {
                return BytecodeLoadError::BadReference;
            }

            // 수신자·대상이 있는 호출은 Ref 창 0번이 있어야 한다. 같은 모듈 함수는 그 매개변수가
            // 창에 들어가야 한다.
            if ((info.op == Opcode::Emit || info.op == Opcode::CallV) && !Window(R, 1)) {
                return BytecodeLoadError::BadRegister;
            }

            const BytecodeRef& ref = module_.refs[bx];
            if (ref.kind == BytecodeRefKind::Function) {
                const BytecodeFunction& callee = module_.functions[ref.detail];
                for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
                    if (!Window(bank, callee.parameters[bank])) {
                        return BytecodeLoadError::BadRegister;
                    }
                }
            }

            return BytecodeLoadError::None;
        }
        case Opcode::Concat:
            return ok(Tuple(B, a, 1) && (c == 0 || Tuple(B, b, c)), BytecodeLoadError::BadRegister);
        case Opcode::EqN:
        case Opcode::NeN:
        case Opcode::EqNF:
        case Opcode::NeNF:
            return ax == 0 || ax > MaxMarkerSlots
                       ? BytecodeLoadError::BadOperand
                       : ok(Tuple(S, a, 1) && Tuple(S, b, ax) && Tuple(S, c, ax),
                            BytecodeLoadError::BadRegister);
        case Opcode::TryS:
            // 원본 Result 묶음 c slot(앞이 오류 코드), 결과 값 c - 1 slot.
            return !target(static_cast<int64>(address) + 2 + DecodeSAx(extra))
                       ? BytecodeLoadError::BadBranch
                   : c == 0 ? BytecodeLoadError::BadOperand
                            : ok(Tuple(S, b, c) && (c == 1 || Tuple(S, a, c - 1)),
                                 BytecodeLoadError::BadRegister);
        case Opcode::TryB:
            return !target(static_cast<int64>(address) + 2 + DecodeSAx(extra))
                       ? BytecodeLoadError::BadBranch
                   : (c >> 5) >= BytecodeBankCount || ((c >> 5) != 0 && (c & 0x1F) > 1)
                       ? BytecodeLoadError::BadOperand
                       : ok(Tuple(B, b, 1) && ((c & 0x1F) == 0 || Tuple(c >> 5, a, c & 0x1F)),
                            BytecodeLoadError::BadRegister);
        case Opcode::MulQ:
        case Opcode::MulM: {
            const uint32 slots = info.op == Opcode::MulQ ? 4 : 16;
            return ok(Tuple(S, a, slots) && Tuple(S, b, slots) && Tuple(S, c, slots),
                      BytecodeLoadError::BadRegister);
        }
        case Opcode::GetFS:
        case Opcode::GetFR:
        case Opcode::GetFB:
        case Opcode::GetSFS:
        case Opcode::GetSFR:
        case Opcode::GetSFB:
        case Opcode::SetFS:
        case Opcode::SetFR:
        case Opcode::SetFB:
        case Opcode::SetSFS:
        case Opcode::SetSFR:
        case Opcode::SetSFB: {
            // C는 필드 값의 slot 수다(Ref·Buf 값은 1).
            const bool get = info.op <= Opcode::GetFB ||
                             (info.op >= Opcode::GetSFS && info.op <= Opcode::GetSFB);
            const bool structField = info.op >= Opcode::GetSFS;
            const uint32 valueBank = bankOf(get ? info.a : info.b);
            if (c == 0 || (valueBank != S && c != 1)) {
                return BytecodeLoadError::BadOperand;
            }

            const bool registers = get ? Tuple(valueBank, a, c) && Tuple(bankOf(info.b), b, 1)
                                       : Tuple(bankOf(info.a), a, 1) && Tuple(valueBank, b, c);
            if (!registers) {
                return BytecodeLoadError::BadRegister;
            }

            if (!Ref(ax, structField ? BytecodeRefKind::StructField : BytecodeRefKind::Field)) {
                return BytecodeLoadError::BadReference;
            }

            // Sparks struct 필드는 TYPE 필드의 bank·slot 수와 맞아야 한다(엔진 struct 필드는 TYPE에
            // 없다 — 바인딩 검사, LC5d).
            if (structField && module_.refs[ax].detail != NoBytecodeIndex) {
                const BytecodeTypeField& field = module_.typeFields[module_.refs[ax].detail];
                if (field.bank != valueBank || field.slots != c) {
                    return BytecodeLoadError::BadOperand;
                }
            }

            return BytecodeLoadError::None;
        }
        case Opcode::AwaitEv:
            return !Tuple(R, a, 1)                    ? BytecodeLoadError::BadRegister
                   : !Ref(ax, BytecodeRefKind::Event) ? BytecodeLoadError::BadReference
                                                      : BytecodeLoadError::None;
        case Opcode::Cast:
            return !Tuple(R, a, 1) || !Tuple(R, b, 1) ? BytecodeLoadError::BadRegister
                   : !Ref(ax, BytecodeRefKind::Type)  ? BytecodeLoadError::BadReference
                                                      : BytecodeLoadError::None;
        case Opcode::ToStr: {
            // 원본 묶음 C slot이 표시 종류와 맞아야 한다(스칼라 1, 수학 타입은 성분 수).
            constexpr uint32 VectorSlots[] = {2, 3, 4, 4, 3, 4, 10, 16};
            const uint32 display = ax >> 16;
            if (display > MaxDisplayKind || (display > static_cast<uint32>(ScalarKind::AssetHi) &&
                                             display < static_cast<uint32>(DisplayKind::Vec2))) {
                return BytecodeLoadError::BadOperand;
            }

            const uint32 expected =
                display >= static_cast<uint32>(DisplayKind::Vec2)
                    ? VectorSlots[display - static_cast<uint32>(DisplayKind::Vec2)]
                    : 1;
            if (c != expected ||
                (display != static_cast<uint32>(ScalarKind::Enum) && (ax & MaxBx) != 0)) {
                return BytecodeLoadError::BadOperand;
            }

            // enum 표시의 Type 참조는 TYPE이 있으면 enum이어야 한다(엔진 enum은 host 이름 표 —
            // LC5d).
            if (display == static_cast<uint32>(ScalarKind::Enum) &&
                (!Ref(ax & MaxBx, BytecodeRefKind::Type) ||
                 (module_.refs[ax & MaxBx].detail != NoBytecodeIndex &&
                  module_.types[module_.refs[ax & MaxBx].detail].kind != BytecodeTypeKind::Enum))) {
                return BytecodeLoadError::BadReference;
            }

            return ok(Tuple(B, a, 1) && Tuple(S, b, c), BytecodeLoadError::BadRegister);
        }
        case Opcode::NewLit: {
            const uint32 literal = ax & MaxBx;
            const uint32 count = (ax >> 16) & 0x3F;
            const uint32 bank = ax >> 22;
            if (!Ref(literal, BytecodeRefKind::Literal)) {
                return BytecodeLoadError::BadReference;
            }

            uint32 perBank[BytecodeBankCount] = {};
            uint32 entries = 0;
            if (bank >= BytecodeBankCount ||
                !LiteralArguments(module_.refs[literal].arguments, perBank, entries) ||
                entries != count) {
                return BytecodeLoadError::BadOperand;
            }

            for (uint32 at = 0; at < BytecodeBankCount; ++at) {
                if (!Window(at, perBank[at])) {
                    return BytecodeLoadError::BadRegister;
                }
            }

            return ok(Tuple(bank, a, 1), BytecodeLoadError::BadRegister);
        }
        case Opcode::GetE:
        case Opcode::SetE:
        case Opcode::GetKey:
        case Opcode::GetVal:
        case Opcode::Append:
        case Opcode::Put: {
            // 컨테이너 표지: 값(bank·slot)과 key(bank·slot). 쓰지 않는 bit는 0이다.
            if (!Marker(ax) || ((ax >> 7) & 1) != 0) {
                return BytecodeLoadError::BadOperand;
            }

            const uint32 valueBank = (ax >> 5) & 0x3;
            const uint32 valueSlots = ax & 0x1F;
            const uint32 keyBank = (ax >> 13) & 0x3;
            const uint32 keySlots = (ax >> 8) & 0x1F;
            bool registers = false;
            switch (info.op) {
            case Opcode::GetE:
                registers = Tuple(valueBank, a, valueSlots) && Tuple(B, b, 1) &&
                            Tuple(keyBank, c, keySlots);
                break;
            case Opcode::SetE:
            case Opcode::Put:
                registers = Tuple(B, a, 1) && Tuple(keyBank, b, keySlots) &&
                            Tuple(valueBank, c, valueSlots);
                break;
            case Opcode::Append:
                registers = Tuple(B, a, 1) && Tuple(valueBank, b, valueSlots);
                break;
            default: // GetKey·GetVal
                registers = Tuple(valueBank, a, valueSlots) && Tuple(B, b, 1) && Tuple(S, c, 1);
                break;
            }

            return ok(registers, BytecodeLoadError::BadRegister);
        }
        case Opcode::Await: {
            const uint32 resultBank = ax >> 8;
            const uint32 slots = ax & 0xFF;
            return resultBank >= BytecodeBankCount || slots > MaxMarkerSlots
                       ? BytecodeLoadError::BadOperand
                       : ok(Tuple(R, b, 1) && (slots == 0 || Tuple(resultBank, a, slots)),
                            BytecodeLoadError::BadRegister);
        }
        case Opcode::Ok: {
            const uint32 valueBank = ax >> 8;
            const uint32 slots = ax & 0xFF;
            return valueBank >= BytecodeBankCount || slots > MaxMarkerSlots ||
                           (valueBank != 0 && slots > 1)
                       ? BytecodeLoadError::BadOperand
                       : ok(Tuple(B, a, 1) && (slots == 0 || Tuple(valueBank, b, slots)),
                            BytecodeLoadError::BadRegister);
        }
        case Opcode::Fault:
            return BytecodeLoadError::None;
        default:
            break;
        }

        // 그 밖: 정해진 bank의 A·B·C 레지스터(slot 하나).
        const bool usesB = info.form == OperandForm::AB || info.form == OperandForm::ABC;
        const bool fits = (info.form == OperandForm::None || info.form == OperandForm::Ax ||
                           info.a == OperandBank::None || Tuple(bankOf(info.a), a, 1)) &&
                          (!usesB || info.b == OperandBank::None || Tuple(bankOf(info.b), b, 1)) &&
                          (info.form != OperandForm::ABC || info.c == OperandBank::None ||
                           Tuple(bankOf(info.c), c, 1));
        return ok(fits, BytecodeLoadError::BadRegister);
    }

    bool IsAwaited(int64 address) const noexcept {
        return (awaited_[address / StartBits] >> (address % StartBits)) & 1;
    }

    bool IsStart(int64 address) const noexcept {
        return (starts_[address / StartBits] >> (address % StartBits)) & 1;
    }

    static constexpr uint32 StartBits = 64;

    const BytecodeModule& module_;
    uint32 function_ = NoBytecodeIndex;
    uint32 frame_[BytecodeBankCount] = {};
    // 함수 명령 시작 표와 await 표에 있는 주소(비트 — 함수 하나는 MaxFunctionWords 단어까지).
    uint64 starts_[MaxFunctionWords / StartBits + 1] = {};
    uint64 awaited_[MaxFunctionWords / StartBits + 1] = {};
    const BytecodeFunction* current_ = nullptr;
};

} // namespace

Result<void> ReadBytecodeFile(Span<const uint8> bytes, BytecodeModule& out,
                              BytecodeRejection& rejection) noexcept {
    rejection = BytecodeRejection{};
    return FileReader(bytes, out, rejection).Read();
}

BytecodeRejection VerifyBytecode(const BytecodeModule& module) noexcept {
    return Verifier(module).Run();
}

} // namespace rocks::sparks
