#include "VmLibrary.h"

#include <Platform/Memory.h>

#include "VmArithmetic.h"

// 언어 함수의 서술자 읽기·묶기·호출이다(Docs/Design/SparksVm.ko.md 6a절, LC5c).

namespace rocks::sparks::detail {

namespace {

// 묶음 하나(참조 번호별)의 배치다: 묶었는지, 구현, 서술자.
struct VmLanguageBinding {
    uint64 bound = 0;
    VmLibraryFunction function = nullptr;
    VmSignature signature;
};

constexpr usize VmBindingWords = (sizeof(VmLanguageBinding) + sizeof(uint64) - 1) / sizeof(uint64);

constexpr uint8 VmScalarBankCode = 0;
constexpr uint8 VmRefBankCode = 1;
constexpr uint8 VmBufBankCode = 2;

// 글자 → 단순 타입이다.
struct VmSimpleCode {
    StringView text;
    VmTypeCode code;
    uint8 bank;
    uint8 slots;
};

constexpr VmSimpleCode VmSimpleCodes[] = {
    {"void", VmTypeCode::Void, 0, 0},    {"b", VmTypeCode::Bool, 0, 1},
    {"i8", VmTypeCode::I8, 0, 1},        {"i16", VmTypeCode::I16, 0, 1},
    {"i32", VmTypeCode::I32, 0, 1},      {"i64", VmTypeCode::I64, 0, 1},
    {"u8", VmTypeCode::U8, 0, 1},        {"u16", VmTypeCode::U16, 0, 1},
    {"u32", VmTypeCode::U32, 0, 1},      {"u64", VmTypeCode::U64, 0, 1},
    {"f32", VmTypeCode::F32, 0, 1},      {"f64", VmTypeCode::F64, 0, 1},
    {"e", VmTypeCode::Enum, 0, 1},       {"n", VmTypeCode::Name, 0, 1},
    {"v2", VmTypeCode::Vec2, 0, 2},      {"v3", VmTypeCode::Vec3, 0, 3},
    {"v4", VmTypeCode::Vec4, 0, 4},      {"q", VmTypeCode::Quat, 0, 4},
    {"r", VmTypeCode::Rotator, 0, 3},    {"c", VmTypeCode::Color, 0, 4},
    {"t", VmTypeCode::Transform, 0, 10}, {"m", VmTypeCode::Mat4, 0, 16},
    {"st", VmTypeCode::Struct, 2, 1},    {"s", VmTypeCode::String, 2, 1},
    {"x", VmTypeCode::Object, 1, 1},     {"w", VmTypeCode::Wait, 1, 1}};

// 서술자 읽기(재귀 하강 — 깊이는 노드 수 상한이 막는다).
class VmSignatureReader final {
public:
    VmSignatureReader(StringView text, VmSignature& out) noexcept : text_(text), out_(out) {
    }

    bool Read() noexcept {
        out_.nodeCount = 0;
        out_.operandCount = 0;
        if (!Peek('>')) {
            for (;;) {
                uint32 node = 0;
                if (out_.operandCount == VmMaxOperands || !Type(node)) {
                    return false;
                }

                out_.operands[out_.operandCount++] = static_cast<uint8>(node);
                if (!Peek(',')) {
                    break;
                }

                ++at_;
            }
        }

        if (!Peek('>')) {
            return false;
        }

        ++at_;
        uint32 result = 0;
        if (!Type(result) || at_ != text_.ByteCount()) {
            return false;
        }

        out_.result = static_cast<uint8>(result);
        // bank별 창 위치(수신자 먼저, 차례로).
        uint16 cursor[BytecodeBankCount] = {};
        for (uint32 index = 0; index < out_.operandCount; ++index) {
            const VmTypeNode& node = out_.nodes[out_.operands[index]];
            out_.offsets[index] = cursor[node.bank];
            cursor[node.bank] = static_cast<uint16>(cursor[node.bank] + node.slots);
        }

        return true;
    }

private:
    bool Peek(char c) const noexcept {
        return at_ < text_.ByteCount() && text_.Data()[at_] == c;
    }

    bool Starts(StringView prefix) const noexcept {
        return at_ + prefix.ByteCount() <= text_.ByteCount() &&
               StringView(text_.Data() + at_, prefix.ByteCount()) == prefix;
    }

    bool Expect(char c) noexcept {
        if (!Peek(c)) {
            return false;
        }

        ++at_;
        return true;
    }

    bool NewNode(uint32& index) noexcept {
        if (out_.nodeCount == VmMaxSignatureNodes) {
            return false;
        }

        index = out_.nodeCount++;
        out_.nodes[index] = VmTypeNode();
        return true;
    }

    bool Type(uint32& index) noexcept {
        if (!NewNode(index)) {
            return false;
        }

        // 묶음 타입: A<…> S<…> M<…,…> R<…> ?<…>.
        const char head = at_ < text_.ByteCount() ? text_.Data()[at_] : '\0';
        if ((head == 'A' || head == 'S' || head == 'M' || head == 'R' || head == '?') &&
            at_ + 1 < text_.ByteCount() && text_.Data()[at_ + 1] == '<') {
            at_ += 2;
            uint32 first = 0;
            if (!Type(first)) {
                return false;
            }

            uint32 second = first;
            if (head == 'M' && (!Expect(',') || !Type(second))) {
                return false;
            }

            if (!Expect('>')) {
                return false;
            }

            VmTypeNode& node = out_.nodes[index];
            node.child[0] = static_cast<uint8>(first);
            node.child[1] = static_cast<uint8>(second);
            const VmTypeNode& inner = out_.nodes[first];
            switch (head) {
            case 'A':
                node.code = VmTypeCode::Array;
                node.bank = VmBufBankCode;
                node.slots = 1;
                break;
            case 'S':
                node.code = VmTypeCode::Set;
                node.bank = VmBufBankCode;
                node.slots = 1;
                break;
            case 'M':
                node.code = VmTypeCode::Map;
                node.bank = VmBufBankCode;
                node.slots = 1;
                break;
            case 'R':
                // Scalar(또는 void) 값의 Result는 Scalar 묶음(코드 + 값), 그 밖은 Buf.
                node.code = VmTypeCode::Result;
                node.bank = inner.bank == VmScalarBankCode ? VmScalarBankCode : VmBufBankCode;
                node.slots =
                    static_cast<uint8>(inner.bank == VmScalarBankCode ? 1 + inner.slots : 1);
                break;
            default:
                node.code = VmTypeCode::Optional;
                node.bank = inner.bank;
                node.slots =
                    static_cast<uint8>(inner.bank == VmScalarBankCode ? 1 + inner.slots : 1);
                break;
            }

            return true;
        }

        // z<bank 글자><slot 수>
        if (head == 'z' && at_ + 2 < text_.ByteCount()) {
            const char bank = text_.Data()[at_ + 1];
            at_ += 2;
            uint32 slots = 0;
            while (at_ < text_.ByteCount() && text_.Data()[at_] >= '0' &&
                   text_.Data()[at_] <= '9') {
                slots = slots * 10 + uint32(text_.Data()[at_] - '0');
                ++at_;
                if (slots > MaxBankRegisters) {
                    return false;
                }
            }

            VmTypeNode& node = out_.nodes[index];
            node.code = VmTypeCode::Raw;
            node.bank = bank == 's'   ? VmScalarBankCode
                        : bank == 'r' ? VmRefBankCode
                                      : VmBufBankCode;
            node.slots = static_cast<uint8>(slots);
            return bank == 's' || bank == 'r' || bank == 'b';
        }

        // 단순 글자(긴 것 먼저 — `st`가 `s`보다, `i16`이 `i8`보다).
        const VmSimpleCode* best = nullptr;
        for (const VmSimpleCode& code : VmSimpleCodes) {
            if (Starts(code.text) &&
                (best == nullptr || code.text.ByteCount() > best->text.ByteCount())) {
                best = &code;
            }
        }

        if (best == nullptr) {
            return false;
        }

        at_ += best->text.ByteCount();
        VmTypeNode& node = out_.nodes[index];
        node.code = best->code;
        node.bank = best->bank;
        node.slots = best->slots;
        return true;
    }

    StringView text_;
    VmSignature& out_;
    usize at_ = 0;
};

} // namespace

bool VmParseSignature(StringView text, VmSignature& out) noexcept {
    return VmSignatureReader(text, out).Read();
}

bool VmIsSigned(VmTypeCode code) noexcept {
    return code >= VmTypeCode::I8 && code <= VmTypeCode::I64;
}

bool VmIsUnsigned(VmTypeCode code) noexcept {
    return code >= VmTypeCode::U8 && code <= VmTypeCode::U64;
}

bool VmIsFloat(VmTypeCode code) noexcept {
    return code == VmTypeCode::F32 || code == VmTypeCode::F64;
}

double VmLibraryContext::Real(uint32 index) const noexcept {
    return VmDouble(Scalars(index)[0]);
}

int64 VmLibraryContext::Integer(uint32 index) const noexcept {
    return static_cast<int64>(Scalars(index)[0]);
}

VmLibraryFunction VmLibrary::Find(StringView name) noexcept {
    const Span<const VmLibraryEntry> tables[] = {VmMathEntries(), VmTextEntries(),
                                                 VmCollectionEntries()};
    for (const Span<const VmLibraryEntry> table : tables) {
        for (const VmLibraryEntry& entry : table) {
            if (entry.name == name) {
                return entry.function;
            }
        }
    }

    return nullptr;
}

VmBuffer* VmLibrary::Allocate(VmLibraryContext& context, VmBufferKind kind, uint64 elements,
                              uint64 elementBytes, VmFaultKind& fault) noexcept {
    const VmExecution::Allocation allocation =
        context.execution->Allocate(kind, elements, elementBytes, context.budget);
    if (allocation.error != ErrorCode::None) {
        context.error = allocation.error;
    }

    fault = allocation.fault;
    return allocation.buffer;
}

VmBuffer* VmLibrary::NewString(VmLibraryContext& context, StringView text,
                               VmFaultKind& fault) noexcept {
    VmBuffer* buffer = Allocate(context, VmBufferKind::String, text.ByteCount(), 1, fault);
    if (buffer != nullptr && text.ByteCount() != 0) {
        platform::MemoryCopy(buffer->Bytes(), text.Data(), text.ByteCount());
    }

    return buffer;
}

void VmLibrary::Release(VmLibraryContext& context, VmBuffer* buffer) noexcept {
    context.execution->Release(buffer);
}

void VmLibrary::Put(VmLibraryContext& context, uint32 at, VmBuffer* value) noexcept {
    VmBuffer* previous = context.b[at];
    context.b[at] = value;
    context.execution->Release(previous);
}

VmFaultKind VmLibrary::Reserve(VmLibraryContext& context, VmBuffer*& slot, uint64 needed,
                               uint32 marker) noexcept {
    return context.execution->Reserve(slot, needed, marker, context.budget, context.error);
}

void VmLibrary::FreeShell(VmLibraryContext& context, VmBuffer* buffer) noexcept {
    context.execution->FreeShell(buffer);
}

RandomStream& VmLibrary::Random(VmLibraryContext& context) noexcept {
    return context.execution->random_;
}

void VmLibrary::Log(VmLibraryContext& context, VmLogLevel level, StringView message) noexcept {
    VmExecution& execution = *context.execution;
    VmExecution::LogEntry entry;
    entry.level = level;
    entry.offset = static_cast<uint32>(execution.logText_.Size());
    entry.length = static_cast<uint32>(message.ByteCount());
    if (!execution.logText_.TryAppend(Span<const char>(message.Data(), message.ByteCount()))) {
        return;
    }

    if (!execution.logs_.TryPushBack(entry)) {
        static_cast<void>(execution.logText_.TryResize(entry.offset));
    }
}

} // namespace rocks::sparks::detail

namespace rocks::sparks {

const char* VmLogLevelName(VmLogLevel level) noexcept {
    switch (level) {
    case VmLogLevel::Info:
        return "info";
    case VmLogLevel::Warning:
        return "warning";
    case VmLogLevel::Error:
        return "error";
    case VmLogLevel::Debug:
        return "debug";
    }

    return "?";
}

VmFaultKind VmExecution::CallLanguage(uint32 ref, const Banks& window, uint64 budget,
                                      ErrorCode& error) noexcept {
    // 처음 부르는 참조는 이름·서술자로 묶어 둔다(모듈이 바뀌면 다시 — SparksVm 6a절).
    using detail::VmLanguageBinding;
    const BytecodeModule& module = *module_;
    if (boundModule_ != module_ ||
        bindings_.Size() != module.refs.Size() * detail::VmBindingWords) {
        if (auto resized = bindings_.TryResize(module.refs.Size() * detail::VmBindingWords);
            !resized) {
            error = resized.Error();
            return VmFaultKind::None;
        }

        for (uint64& word : bindings_) {
            word = 0;
        }

        boundModule_ = module_;
    }

    VmLanguageBinding& binding = *reinterpret_cast<VmLanguageBinding*>(
        bindings_.Data() + usize(ref) * detail::VmBindingWords);
    if (binding.bound == 0) {
        const BytecodeRef& entry = module.refs[ref];
        binding.function = detail::VmLibrary::Find(module.String(entry.name));
        if (binding.function == nullptr || entry.arguments == NoBytecodeIndex ||
            !detail::VmParseSignature(module.String(entry.arguments), binding.signature)) {
            return VmFaultKind::ContractViolation; // 모르는 언어 함수(LC5d·LC6 또는 판 어긋남)
        }

        binding.bound = 1;
    }

    detail::VmLibraryContext context;
    context.execution = this;
    context.signature = &binding.signature;
    context.s = window.s;
    context.r = window.r;
    context.b = window.b;
    context.budget = budget;
    const VmFaultKind fault = binding.function(context);
    error = context.error;
    return fault;
}

} // namespace rocks::sparks
