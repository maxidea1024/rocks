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
constexpr uint8 FunctionsTag[TagBytes] = {'F', 'U', 'N', 'C'};
constexpr uint8 DebugTag[TagBytes] = {'D', 'B', 'U', 'G'};

// 표 원소의 최소 byte 수다(개수를 믿기 전에 남은 byte와 비교한다 — 큰 할당 막기).
constexpr usize ScalarBytes = 8;
constexpr usize StringHeaderBytes = 4;
constexpr usize RefBytes = 9;
constexpr usize FunctionBytes = 4 * 3 + 2 * 3 * BytecodeBankCount + 4 * 4;
constexpr usize WordBytes = 4;
constexpr usize SwitchBytes = 12;
constexpr usize CaseBytes = 12;
constexpr usize AwaitBytes = 12;
constexpr usize LiveRangeBytes = 5;
constexpr usize LocationBytes = 8;

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
           op == Opcode::RetErrB || op == Opcode::Fault;
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

        if (auto read = Section(ConstantsTag, &FileReader::Constants); !read) {
            return read;
        }

        if (auto read = Section(ReferencesTag, &FileReader::References); !read) {
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
                return Reject(BytecodeLoadError::Truncated);
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
            const bool detailOk =
                ref.kind == BytecodeRefKind::Literal
                    ? ref.detail == NoBytecodeIndex || ref.detail < strings
                : ref.kind == BytecodeRefKind::Function
                    ? ref.detail == NoBytecodeIndex || ref.detail < module_.functions.Size()
                    : true;
            if (ref.name >= strings || !detailOk) {
                return Reject(BytecodeLoadError::BadTable);
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
        const uint32 valueBank = (marker >> 5) & 0x3;
        const uint32 keyBank = (marker >> 13) & 0x3;
        return valueBank < BytecodeBankCount && keyBank < BytecodeBankCount &&
               (marker & 0x1F) <= MaxMarkerSlots && ((marker >> 8) & 0x1F) <= MaxMarkerSlots &&
               (marker >> 15) == 0;
    }

    BytecodeRejection Function(const BytecodeFunction& function) noexcept {
        if (function.name >= module_.stringOffsets.Size() ||
            usize(function.firstWord) + function.wordCount > module_.words.Size() ||
            function.wordCount == 0 || function.wordCount > MaxFunctionWords ||
            usize(function.firstSwitch) + function.switchCount > module_.switches.Size() ||
            usize(function.firstAwait) + function.awaitCount > module_.awaits.Size()) {
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

        // 명령 시작 표(EXTRA 단어는 시작이 아니다).
        const uint32* words = module_.words.Data() + function.firstWord;
        for (uint64& bits : starts_) {
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
        for (uint32 address = 0; address < function.wordCount; ++address) {
            const uint32 word = words[address];
            const OpcodeInfo& info = InfoOf(DecodeOpcode(word));
            const uint32 extra = info.extra ? words[address + 1] : 0;
            if (const BytecodeLoadError error = Instruction(info, word, extra, address, target);
                error != BytecodeLoadError::None) {
                return Reject(error, address);
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

        for (uint32 index = function.firstAwait; index < function.firstAwait + function.awaitCount;
             ++index) {
            const BytecodeAwaitPoint& point = module_.awaits[index];
            if (!target(point.address) || (DecodeOpcode(words[point.address]) != Opcode::Await &&
                                           DecodeOpcode(words[point.address]) != Opcode::AwaitEv)) {
                return Reject(BytecodeLoadError::BadBranch, point.address);
            }

            for (uint32 at = point.firstLive; at < point.firstLive + point.liveCount; ++at) {
                const BytecodeLiveRange& range = module_.liveRanges[at];
                if (static_cast<uint32>(range.bank) >= BytecodeBankCount ||
                    range.first + range.count >
                        function.registers[static_cast<uint32>(range.bank)]) {
                    return Reject(BytecodeLoadError::BadRegister, point.address);
                }
            }
        }

        return BytecodeRejection{};
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
        switch (info.op) {
        case Opcode::LoadK:
            return !Register(info.a, a)           ? BytecodeLoadError::BadRegister
                   : bx >= module_.scalars.Size() ? BytecodeLoadError::BadConstant
                                                  : BytecodeLoadError::None;
        case Opcode::LoadStr:
            return !Register(info.a, a)                 ? BytecodeLoadError::BadRegister
                   : bx >= module_.stringOffsets.Size() ? BytecodeLoadError::BadConstant
                                                        : BytecodeLoadError::None;
        case Opcode::Jmp:
            return target(next + DecodeSBx(word)) ? BytecodeLoadError::None
                                                  : BytecodeLoadError::BadBranch;
        case Opcode::Brt:
        case Opcode::Brf:
            return !Register(info.a, a)              ? BytecodeLoadError::BadRegister
                   : !target(next + DecodeSBx(word)) ? BytecodeLoadError::BadBranch
                                                     : BytecodeLoadError::None;
        case Opcode::Switch:
            return !Register(info.a, a) ? BytecodeLoadError::BadRegister
                   : bx < module_.functions[function_].firstSwitch ||
                           bx >= module_.functions[function_].firstSwitch +
                                     module_.functions[function_].switchCount
                       ? BytecodeLoadError::BadBranch
                       : BytecodeLoadError::None;
        case Opcode::Ret:
            if (b == 0) {
                return BytecodeLoadError::None;
            }

            return c >= BytecodeBankCount || !Register(static_cast<OperandBank>(c + 1), a, b)
                       ? BytecodeLoadError::BadRegister
                       : BytecodeLoadError::None;
        case Opcode::RetErrS:
            return Register(OperandBank::Scalar, a, b == 0 ? 1 : b)
                       ? BytecodeLoadError::None
                       : BytecodeLoadError::BadRegister;
        case Opcode::Conv:
            return !Register(info.a, a) || !Register(info.b, b) ? BytecodeLoadError::BadRegister
                   : (c >> 4) > static_cast<uint32>(ScalarKind::AssetHi) ||
                           (c & 0xF) > static_cast<uint32>(ScalarKind::AssetHi)
                       ? BytecodeLoadError::BadOperand
                       : BytecodeLoadError::None;
        case Opcode::GetCtx:
            return !Register(info.a, a)                 ? BytecodeLoadError::BadRegister
                   : !Ref(bx, BytecodeRefKind::Context) ? BytecodeLoadError::BadReference
                                                        : BytecodeLoadError::None;
        case Opcode::NewS:
        case Opcode::NewC:
        case Opcode::NewA:
        case Opcode::NewSet:
        case Opcode::NewM:
            return !Register(info.a, a)              ? BytecodeLoadError::BadRegister
                   : !Ref(bx, BytecodeRefKind::Type) ? BytecodeLoadError::BadReference
                                                     : BytecodeLoadError::None;
        case Opcode::Call:
        case Opcode::CallV:
        case Opcode::CallN:
        case Opcode::CallL:
        case Opcode::CallRpc:
        case Opcode::Emit:
            return CallRef(info.op, bx) ? BytecodeLoadError::None : BytecodeLoadError::BadReference;
        case Opcode::Concat:
            return Register(info.a, a) && Register(OperandBank::Buf, b, c)
                       ? BytecodeLoadError::None
                       : BytecodeLoadError::BadRegister;
        case Opcode::EqN:
        case Opcode::NeN:
            return ax == 0 || ax > MaxMarkerSlots ? BytecodeLoadError::BadOperand
                   : !Register(info.a, a) || !Register(info.b, b, ax) || !Register(info.c, c, ax)
                       ? BytecodeLoadError::BadRegister
                       : BytecodeLoadError::None;
        case Opcode::TryS:
            return !target(static_cast<int64>(address) + 2 + DecodeSAx(extra))
                       ? BytecodeLoadError::BadBranch
                   : !Register(OperandBank::Scalar, b, c == 0 ? 1 : c)
                       ? BytecodeLoadError::BadRegister
                       : BytecodeLoadError::None;
        case Opcode::TryB:
            return !target(static_cast<int64>(address) + 2 + DecodeSAx(extra))
                       ? BytecodeLoadError::BadBranch
                   : !Register(info.b, b) ? BytecodeLoadError::BadRegister
                                          : BytecodeLoadError::None;
        case Opcode::GetFS:
        case Opcode::GetFR:
        case Opcode::GetFB:
        case Opcode::SetFS:
        case Opcode::SetFR:
        case Opcode::SetFB:
            return !Register(info.a, a) || !Register(info.b, b) ? BytecodeLoadError::BadRegister
                   : !Ref(ax, BytecodeRefKind::Field)           ? BytecodeLoadError::BadReference
                                                                : BytecodeLoadError::None;
        case Opcode::GetSFS:
        case Opcode::GetSFR:
        case Opcode::GetSFB:
        case Opcode::SetSFS:
        case Opcode::SetSFR:
        case Opcode::SetSFB:
            return !Register(info.a, a) || !Register(info.b, b) ? BytecodeLoadError::BadRegister
                   : !Ref(ax, BytecodeRefKind::StructField)     ? BytecodeLoadError::BadReference
                                                                : BytecodeLoadError::None;
        case Opcode::AwaitEv:
            return !Register(info.a, a)               ? BytecodeLoadError::BadRegister
                   : !Ref(ax, BytecodeRefKind::Event) ? BytecodeLoadError::BadReference
                                                      : BytecodeLoadError::None;
        case Opcode::Cast:
            return !Register(info.a, a) || !Register(info.b, b) ? BytecodeLoadError::BadRegister
                   : !Ref(ax, BytecodeRefKind::Type)            ? BytecodeLoadError::BadReference
                                                                : BytecodeLoadError::None;
        case Opcode::ToStr: {
            const uint32 display = ax >> 16;
            if (display > MaxDisplayKind) {
                return BytecodeLoadError::BadOperand;
            }

            if (display == static_cast<uint32>(ScalarKind::Enum) &&
                !Ref(ax & MaxBx, BytecodeRefKind::Type)) {
                return BytecodeLoadError::BadReference;
            }

            return Register(info.a, a) && Register(info.b, b) ? BytecodeLoadError::None
                                                              : BytecodeLoadError::BadRegister;
        }
        case Opcode::NewLit:
            return Ref(ax & MaxBx, BytecodeRefKind::Literal) ? BytecodeLoadError::None
                                                             : BytecodeLoadError::BadReference;
        case Opcode::GetE:
        case Opcode::SetE:
        case Opcode::GetKey:
        case Opcode::GetVal:
        case Opcode::Append:
        case Opcode::Put:
            return !Marker(ax) ? BytecodeLoadError::BadOperand
                   : !Register(info.a, a) || !Register(info.b, b) || !Register(info.c, c)
                       ? BytecodeLoadError::BadRegister
                       : BytecodeLoadError::None;
        case Opcode::Await:
            return (ax >> 8) >= BytecodeBankCount || (ax & 0xFF) > MaxMarkerSlots
                       ? BytecodeLoadError::BadOperand
                   : !Register(info.b, b) ? BytecodeLoadError::BadRegister
                   : (ax & 0xFF) != 0 &&
                           !Register(static_cast<OperandBank>((ax >> 8) + 1), a, ax & 0xFF)
                       ? BytecodeLoadError::BadRegister
                       : BytecodeLoadError::None;
        case Opcode::Ok:
            return (ax >> 8) >= BytecodeBankCount ? BytecodeLoadError::BadOperand
                   : !Register(info.a, a)         ? BytecodeLoadError::BadRegister
                                                  : BytecodeLoadError::None;
        case Opcode::Fault:
            return BytecodeLoadError::None;
        default:
            break;
        }

        // 그 밖: 정해진 bank의 A·B·C 레지스터.
        const bool usesB = info.form == OperandForm::AB || info.form == OperandForm::ABC;
        const bool ok = (info.form == OperandForm::None || info.form == OperandForm::Ax ||
                         Register(info.a, a)) &&
                        (!usesB || Register(info.b, b)) &&
                        (info.form != OperandForm::ABC || Register(info.c, c));
        return ok ? BytecodeLoadError::None : BytecodeLoadError::BadRegister;
    }

    bool IsStart(int64 address) const noexcept {
        return (starts_[address / StartBits] >> (address % StartBits)) & 1;
    }

    static constexpr uint32 StartBits = 64;

    const BytecodeModule& module_;
    uint32 function_ = NoBytecodeIndex;
    uint32 frame_[BytecodeBankCount] = {};
    // 함수 명령 시작 표(비트 — 함수 하나는 MaxFunctionWords 단어까지).
    uint64 starts_[MaxFunctionWords / StartBits + 1] = {};
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
