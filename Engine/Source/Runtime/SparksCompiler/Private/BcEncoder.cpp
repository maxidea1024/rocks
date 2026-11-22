#include "BcEncoder.h"

#include <Core/Algorithms.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Fatal.h>

#include "CompilationChecker.h"
#include "IrBuilder.h"
#include "StringLiteralReader.h"

// typed IR → bytecode(Docs/Design/SparksBytecode.ko.md 4절, LC4b): 모듈 표, 생존 분석과 선형 스캔
// 할당, 블록 배치와 명령 선택이다.

namespace rocks::sparks::detail {

namespace {

constexpr uint32 NoPosition = NumericLimits<uint32>::Max();
constexpr uint32 BitsPerWord = 64;
constexpr uint32 WidthCount = 8; // I8…I64, U8…U64

uint32 BankIndex(IrBank bank) noexcept {
    return bank == IrBank::Scalar ? 0 : bank == IrBank::Ref ? 1 : 2;
}

ScalarKind KindOf(IrScalar scalar) noexcept {
    switch (scalar) {
    case IrScalar::Bool:
        return ScalarKind::Bool;
    case IrScalar::I8:
        return ScalarKind::I8;
    case IrScalar::I16:
        return ScalarKind::I16;
    case IrScalar::I32:
        return ScalarKind::I32;
    case IrScalar::I64:
        return ScalarKind::I64;
    case IrScalar::U8:
        return ScalarKind::U8;
    case IrScalar::U16:
        return ScalarKind::U16;
    case IrScalar::U32:
        return ScalarKind::U32;
    case IrScalar::U64:
        return ScalarKind::U64;
    case IrScalar::F32:
        return ScalarKind::F32;
    case IrScalar::F64:
        return ScalarKind::F64;
    case IrScalar::Enum:
        return ScalarKind::Enum;
    case IrScalar::Name:
        return ScalarKind::Name;
    case IrScalar::Flag:
        return ScalarKind::Flag;
    case IrScalar::AssetLo:
        return ScalarKind::AssetLo;
    case IrScalar::AssetHi:
        return ScalarKind::AssetHi;
    case IrScalar::None:
        break;
    }

    return ScalarKind::I64;
}

// 정수 산술의 폭 번호(I8…I64 = 0…3, U8…U64 = 4…7)다. bool·enum·이름·오류 코드는 I64로 센다.
uint32 WidthOf(IrScalar scalar) noexcept {
    switch (scalar) {
    case IrScalar::I8:
        return 0;
    case IrScalar::I16:
        return 1;
    case IrScalar::I32:
        return 2;
    case IrScalar::U8:
        return 4;
    case IrScalar::U16:
        return 5;
    case IrScalar::U32:
        return 6;
    case IrScalar::U64:
        return 7;
    default:
        return 3;
    }
}

bool IsFloat(IrScalar scalar) noexcept {
    return scalar == IrScalar::F32 || scalar == IrScalar::F64;
}

bool IsUnsigned(IrScalar scalar) noexcept {
    return scalar == IrScalar::U8 || scalar == IrScalar::U16 || scalar == IrScalar::U32 ||
           scalar == IrScalar::U64;
}

Opcode Offset(Opcode base, uint32 offset) noexcept {
    return static_cast<Opcode>(static_cast<uint32>(base) + offset);
}

} // namespace

BytecodeEncoder::BytecodeEncoder(TypeChecker& checker, uint32 module, BytecodeModule& out) noexcept
    : checker_(checker), c_(checker.compilation_), module_(module), out_(out),
      scalarIndex_(checker.allocator_), stringIndex_(checker.allocator_),
      stringNext_(checker.allocator_), refIndex_(checker.allocator_),
      functionIndex_(checker.allocator_), typeIndex_(checker.allocator_),
      fieldIndex_(checker.allocator_), order_(checker.allocator_), layoutOf_(checker.allocator_),
      firstPos_(checker.allocator_), start_(checker.allocator_), end_(checker.allocator_),
      physFirst_(checker.allocator_), stringOf_(checker.allocator_),
      blockAddress_(checker.allocator_), fixups_(checker.allocator_),
      switchFixups_(checker.allocator_) {
}

bool BytecodeEncoder::Fail(ErrorCode error) noexcept {
    checker_.binder_.error_ = error;
    return false;
}

void BytecodeEncoder::Declare(uint32 symbol, uint32 index) noexcept {
    if (auto inserted = functionIndex_.Insert(uint64(symbol), index); !inserted) {
        Fail(inserted.Error());
    }
}

// ---- 모듈 표 ----

uint32 BytecodeEncoder::AddScalar(uint64 bits) noexcept {
    if (checker_.binder_.error_ != ErrorCode::None) {
        return 0; // 실패 뒤에는 표를 더 바꾸지 않는다(배열 길이가 어긋나지 않게)
    }

    if (const uint32* found = scalarIndex_.Find(bits)) {
        return *found;
    }

    const uint32 index = static_cast<uint32>(out_.scalars.Size());
    if (auto pushed = out_.scalars.TryPushBack(bits); !pushed) {
        Fail(pushed.Error());
        return 0;
    }

    if (auto inserted = scalarIndex_.Insert(bits, index); !inserted) {
        Fail(inserted.Error());
    }

    return index;
}

uint32 BytecodeEncoder::AddString(StringView text) noexcept {
    if (checker_.binder_.error_ != ErrorCode::None) {
        return 0;
    }

    const uint64 hash = Hash<StringView>{}(text);
    const uint32* head = stringIndex_.Find(hash);
    for (uint32 at = head == nullptr ? NoBytecodeIndex : *head; at != NoBytecodeIndex;
         at = stringNext_[at]) {
        if (out_.String(at) == text) {
            return at;
        }
    }

    const uint32 index = static_cast<uint32>(out_.stringOffsets.Size());
    const uint32 offset = static_cast<uint32>(out_.stringBytes.Size());
    if (!out_.stringBytes.TryAppend(Span<const char>(text.Data(), text.ByteCount())) ||
        !out_.stringOffsets.TryPushBack(offset) ||
        !out_.stringLengths.TryPushBack(static_cast<uint32>(text.ByteCount())) ||
        !stringNext_.TryPushBack(head == nullptr ? NoBytecodeIndex : *head)) {
        // 한 항목의 네 배열을 함께 되돌린다(축소는 실패하지 않는다).
        static_cast<void>(out_.stringBytes.TryResize(offset));
        static_cast<void>(out_.stringOffsets.TryResize(index));
        static_cast<void>(out_.stringLengths.TryResize(index));
        static_cast<void>(stringNext_.TryResize(index));
        Fail(ErrorCode::OutOfMemory);
        return 0;
    }

    if (auto assigned = stringIndex_.InsertOrAssign(hash, index); !assigned) {
        Fail(assigned.Error());
    }

    return index;
}

uint32 BytecodeEncoder::AddDecodedString(StringView text, bool piece) noexcept {
    // 푼 byte를 모은다(UTF-8 code point를 byte 단위로 내므로 서식 함수가 아니라 byte 배열에
    // 쌓는다).
    Array<char> decoded(checker_.allocator_);
    StringLiteralReader reader =
        piece ? StringLiteralReader::ForPiece(text) : StringLiteralReader(text);
    uint8 byte = 0;
    while (reader.Next(byte)) {
        if (auto pushed = decoded.TryPushBack(static_cast<char>(byte)); !pushed) {
            Fail(pushed.Error());
            return 0;
        }
    }

    return AddString(StringView(decoded.Data(), decoded.Size()));
}

uint32 BytecodeEncoder::AddRef(BytecodeRefKind kind, uint32 name, uint32 detail,
                               uint32 arguments) noexcept {
    if (checker_.binder_.error_ != ErrorCode::None) {
        return 0;
    }

    const uint32 distinct = arguments != NoBytecodeIndex ? arguments : detail;
    const uint64 key =
        (uint64(kind) << 56) | (uint64(name & 0x0FFFFFFF) << 28) | uint64(distinct & 0x0FFFFFFF);
    if (const uint32* found = refIndex_.Find(key)) {
        return *found;
    }

    const uint32 index = static_cast<uint32>(out_.refs.Size());
    if (auto pushed = out_.refs.TryPushBack(BytecodeRef{kind, name, detail, arguments}); !pushed) {
        Fail(pushed.Error());
        return 0;
    }

    if (auto inserted = refIndex_.Insert(key, index); !inserted) {
        Fail(inserted.Error());
    }

    return index;
}

bool BytecodeEncoder::Qualify(StringBuilderBase& name, uint32 symbol) noexcept {
    // 다른 모듈의 Sparks 선언은 `<import 경로>:` 접두로 구별한다(같은 이름의 두 모듈 선언).
    if (symbol == NoSymbol || symbol >= c_.symbols_.Size()) {
        return false;
    }

    const Symbol& entry = c_.symbols_[symbol];
    switch (entry.kind) {
    case SymbolKind::Class:
    case SymbolKind::Struct:
    case SymbolKind::Enum:
    case SymbolKind::Interface:
    case SymbolKind::Const:
    case SymbolKind::Function:
    case SymbolKind::Field:
    case SymbolKind::Method:
    case SymbolKind::Event:
    case SymbolKind::EnumCase:
    case SymbolKind::InterfaceMethod:
        break;
    default:
        return false;
    }

    if (entry.module >= c_.modules_.Size() || entry.module == module_) {
        return false;
    }

    name.Append(c_.Text(c_.modules_[entry.module].importPath)).Append(":");
    return true;
}

uint32 BytecodeEncoder::SymbolRef(BytecodeRefKind kind, uint32 symbol, uint32 types,
                                  uint32 arguments) noexcept {
    // 표시 이름은 `[모듈:]Owner.Name[<타입 인자>]`이다(LC4d가 안정 ID를 더한다).
    StringBuilder<128> name(checker_.allocator_);
    const Symbol& entry = c_.symbols_[symbol];
    if (!Qualify(name, symbol) && entry.owner != NoSymbol) {
        Qualify(name, entry.owner);
    }

    if (entry.owner != NoSymbol && entry.owner < c_.symbols_.Size()) {
        name.Append(c_.symbols_[entry.owner].name).Append(".");
    }

    name.Append(entry.name);
    uint32 detail = NoBytecodeIndex;
    if (types != NoIrRegister) {
        // 명시한 타입 인자: 이름에 적고 첫 타입 인자의 Type 참조를 보조 번호로 둔다.
        const uint32 count = f_->operands[types];
        name.Append("<");
        for (uint32 index = 0; index < count; ++index) {
            if (index != 0) {
                name.Append(", ");
            }

            checker_.AppendDisplayType(name, f_->operands[types + 1 + index]);
        }

        name.Append(">");
        detail = count > 0 ? TypeRef(f_->operands[types + 1]) : NoBytecodeIndex;
    }

    if (name.HasFailed()) {
        Fail(name.Status().Error());
        return 0;
    }

    if (kind == BytecodeRefKind::Function) {
        const uint32* found = functionIndex_.Find(uint64(symbol));
        detail = found == nullptr ? NoBytecodeIndex : *found;
    }

    // Sparks struct의 필드는 TYPE 필드 번호를 보조 번호로 둔다(엔진 struct 필드는 없음 — LC5d).
    if (kind == BytecodeRefKind::StructField && entry.owner != NoSymbol &&
        TypeTable(entry.owner) != NoBytecodeIndex) {
        const uint32* found = fieldIndex_.Find(uint64(symbol));
        detail = found == nullptr ? NoBytecodeIndex : *found;
    }

    // native 함수·객체 필드는 host가 링크 때 대조할 서술자를 싣는다(SparksVm 6.1절).
    // 엔진 struct 필드(TYPE 필드 없음)·가상 함수도 host가 대조한다(6.5절).
    if (arguments == NoBytecodeIndex &&
        (kind == BytecodeRefKind::NativeFunction || kind == BytecodeRefKind::Field ||
         kind == BytecodeRefKind::ImportedFunction || kind == BytecodeRefKind::VirtualFunction ||
         (kind == BytecodeRefKind::StructField && detail == NoBytecodeIndex))) {
        arguments = HostDescriptor(kind, symbol);
    }

    return AddRef(kind, AddString(name.View()), detail, arguments);
}

uint32 BytecodeEncoder::HostDescriptor(BytecodeRefKind kind, uint32 symbol) noexcept {
    // 필드는 `>타입`, native 함수는 선언 시그니처(수신자 `x` 먼저 — static이면 없음)다. 타입 인자는
    // 객체 제약이라 `x`다(AppendDescriptor).
    StringBuilder<128> descriptor(checker_.allocator_);
    const Symbol& entry = c_.symbols_[symbol];
    if (kind == BytecodeRefKind::Field || kind == BytecodeRefKind::StructField) {
        descriptor.Append(">");
        // Sparks 필드는 초기값 추론이 있을 수 있고, 엔진 필드는 schema 타입이 심볼에 있다.
        AppendDescriptor(descriptor,
                         entry.kind == SymbolKind::Field ? checker_.FieldType(symbol) : entry.type);
    } else if (entry.signature != NoSignature) {
        const Signature& signature = c_.signatures_[entry.signature];
        // 수신자: 엔진 함수는 static이 아니면, Sparks 함수는 메서드면 `x`(this)다.
        bool first =
            entry.kind == SymbolKind::EngineFunction
                ? (entry.flags & SymbolFlags::Static) != 0
                : entry.kind != SymbolKind::Method && entry.kind != SymbolKind::InterfaceMethod;
        if (!first) {
            // struct 메서드의 this는 struct 값(Buf `st`)이다(LC5d2 리뷰 B4).
            const bool structOwner =
                entry.owner != NoSymbol && c_.symbols_[entry.owner].kind == SymbolKind::Struct;
            descriptor.Append(structOwner ? "st" : "x");
        }

        for (uint32 index = 0; index < signature.parameterCount; ++index) {
            descriptor.Append(first ? "" : ",");
            first = false;
            AppendDescriptor(descriptor, c_.signatureParameters_[signature.firstParameter + index]);
        }

        descriptor.Append(">");
        AppendDescriptor(descriptor, signature.returnType);
    }

    if (descriptor.HasFailed()) {
        Fail(descriptor.Status().Error());
        return NoBytecodeIndex;
    }

    return AddString(descriptor.View());
}

void BytecodeEncoder::AppendDescriptor(StringBuilderBase& out, uint32 type) noexcept {
    // 타입 글자(SparksVm 6a절). 모르는 타입은 배치(`z<bank 글자><slot 수>`)로 쓴다.
    if (type == NoType || type == checker_.voidType_) {
        out.Append("void");
        return;
    }

    if (checker_.IsOptional(type)) {
        out.Append("?<");
        AppendDescriptor(out, checker_.NonOptional(type));
        out.Append(">");
        return;
    }

    const uint32 symbol = checker_.SymbolOf(type);
    const Symbol* entry = symbol == NoSymbol ? nullptr : &c_.symbols_[symbol];
    const StringView name = entry == nullptr ? StringView() : entry->name;
    struct Letter {
        StringView name;
        StringView code;
    };
    constexpr Letter Letters[] = {
        {"bool", "b"},     {"int8", "i8"},    {"int16", "i16"},   {"int32", "i32"},
        {"int64", "i64"},  {"int", "i64"},    {"uint8", "u8"},    {"uint16", "u16"},
        {"uint32", "u32"}, {"uint64", "u64"}, {"float32", "f32"}, {"float64", "f64"},
        {"float", "f64"},  {"vec2", "v2"},    {"vec3", "v3"},     {"vec4", "v4"},
        {"quat", "q"},     {"Rotator", "r"},  {"Color", "c"},     {"Transform", "t"},
        {"mat4", "m"},     {"string", "s"},   {"Tag", "n"},       {"Clock", "e"}};
    if (entry != nullptr &&
        (entry->kind == SymbolKind::BuiltinType || entry->kind == SymbolKind::PreludeType)) {
        for (const Letter& letter : Letters) {
            if (letter.name == name) {
                out.Append(letter.code);
                return;
            }
        }

        const TypeEntry& shape = c_.types_[type];
        auto argument = [&](uint32 index) noexcept {
            AppendDescriptor(out, c_.typeArguments_[shape.firstArgument + index]);
        };
        if ((name == "Array" || name == "Set") && shape.argumentCount == 1) {
            out.Append(name == "Array" ? "A<" : "S<");
            argument(0);
            out.Append(">");
            return;
        }

        if (name == "Map" && shape.argumentCount == 2) {
            out.Append("M<");
            argument(0);
            out.Append(",");
            argument(1);
            out.Append(">");
            return;
        }

        if (name == "Result" && shape.argumentCount == 1) {
            out.Append("R<");
            argument(0);
            out.Append(">");
            return;
        }
    }

    if (entry != nullptr && entry->kind == SymbolKind::Enum) {
        out.Append("e");
        return;
    }

    if (entry != nullptr && entry->kind == SymbolKind::Struct) {
        out.Append("st");
        return;
    }

    if (entry != nullptr && entry->kind == SymbolKind::EngineTypeParameter) {
        out.Append("x"); // schema 타입 매개변수의 제약은 객체 타입이다
        return;
    }

    if (entry != nullptr && entry->kind == SymbolKind::EngineType &&
        entry->index < c_.schemas_.Size()) {
        const SchemaTypeKind schemaKind =
            c_.schemas_[entry->index].schema.Types()[entry->item].kind;
        if (schemaKind == SchemaTypeKind::Enum) {
            out.Append("e");
            return;
        }

        if (schemaKind == SchemaTypeKind::Struct) {
            out.Append("st");
            return;
        }
    }

    const IrLayout layout = IrBuilder::Layout(checker_, type);
    if (layout.bank == IrBank::Ref && layout.slots == 1) {
        out.Append("x");
        return;
    }

    out.AppendFormat("z{}{}",
                     layout.bank == IrBank::Scalar ? 's'
                     : layout.bank == IrBank::Ref  ? 'r'
                                                   : 'b',
                     layout.slots);
}

uint32 BytecodeEncoder::TypeRef(uint32 type) noexcept {
    StringBuilder<128> name(checker_.allocator_);
    Qualify(name, checker_.SymbolOf(checker_.NonOptional(type)));
    checker_.AppendDisplayType(name, type);
    if (name.HasFailed()) {
        Fail(name.Status().Error());
        return 0;
    }

    return AddRef(BytecodeRefKind::Type, AddString(name.View()),
                  TypeTable(checker_.SymbolOf(checker_.NonOptional(type))));
}

uint32 BytecodeEncoder::TypeTable(uint32 symbol) noexcept {
    // Sparks struct·enum과 언어 소유 enum `Clock`(case는 언어 표 순 — TOSTR가 이름을 쓴다).
    // 엔진 enum·struct는 host 이름 표·배치(LC5d)라 TYPE에 없다.
    const bool clock = symbol != NoSymbol && symbol < c_.symbols_.Size() &&
                       c_.symbols_[symbol].kind == SymbolKind::PreludeType &&
                       c_.symbols_[symbol].name == "Clock";
    if (symbol == NoSymbol || symbol >= c_.symbols_.Size() ||
        (c_.symbols_[symbol].kind != SymbolKind::Struct &&
         c_.symbols_[symbol].kind != SymbolKind::Enum && !clock)) {
        return NoBytecodeIndex;
    }

    if (const uint32* found = typeIndex_.Find(uint64(symbol))) {
        return *found;
    }

    // 이름은 Type 참조와 같은 표시 이름이다.
    const Symbol entry = c_.symbols_[symbol];
    StringBuilder<128> name(checker_.allocator_);
    Qualify(name, symbol);
    checker_.AppendDisplayType(name, checker_.TypeOfSymbol(symbol));
    if (name.HasFailed()) {
        Fail(name.Status().Error());
        return NoBytecodeIndex;
    }

    BytecodeType type;
    type.name = AddString(name.View());
    type.kind =
        entry.kind == SymbolKind::Struct ? BytecodeTypeKind::Struct : BytecodeTypeKind::Enum;
    type.firstField = static_cast<uint32>(out_.typeFields.Size());
    const uint32 index = static_cast<uint32>(out_.types.Size());
    if (!out_.types.TryPushBack(type) || !typeIndex_.Insert(uint64(symbol), index)) {
        Fail(ErrorCode::OutOfMemory);
        return NoBytecodeIndex;
    }

    // 필드(case)는 선언 순이다. struct 필드는 bank 묶음(Scalar·Ref·Buf) 안의 절대 slot 위치를
    // 받는다.
    Array<uint32> fields(checker_.allocator_);
    if (clock) {
        for (uint32 at = 0; at < c_.symbols_.Size(); ++at) {
            if (c_.symbols_[at].kind == SymbolKind::EnumCase && c_.symbols_[at].owner == symbol &&
                !fields.TryPushBack(at)) {
                Fail(ErrorCode::OutOfMemory);
                return NoBytecodeIndex;
            }
        }
    }

    const SyntaxTree* tree = clock ? nullptr : &c_.trees_[entry.module];
    const uint32 members = clock ? NoNode : tree->Child(entry.node, 3);
    if (members != NoNode && tree->Node(members).kind == SyntaxKind::List) {
        for (const uint32 member : tree->Children(members)) {
            const SyntaxKind kind = tree->Node(member).kind;
            if (kind != SyntaxKind::Field && kind != SyntaxKind::EnumCase) {
                continue;
            }

            const uint32 field = c_.NodeSymbol(entry.module, tree->Child(member, 2));
            if (field != NoSymbol && !fields.TryPushBack(field)) {
                Fail(ErrorCode::OutOfMemory);
                return NoBytecodeIndex;
            }
        }
    }

    uint32 slots[BytecodeBankCount] = {};
    for (const uint32 field : fields) {
        BytecodeTypeField item;
        item.name = AddString(c_.symbols_[field].name);
        if (type.kind == BytecodeTypeKind::Struct) {
            const IrLayout layout = IrBuilder::Layout(checker_, checker_.FieldType(field));
            item.bank = static_cast<uint8>(layout.bank);
            item.slots = static_cast<uint8>(layout.slots);
            item.offset = slots[item.bank]; // 묶음 안 위치 — 아래에서 절대 위치로 옮긴다
            slots[item.bank] += layout.slots;
            if (slots[item.bank] > NumericLimits<uint16>::Max()) {
                TooLarge(); // struct 하나의 bank별 slot 합은 16비트(TYPE 구역) — S1150
                return NoBytecodeIndex;
            }
        }

        if (!fieldIndex_.Insert(uint64(field), static_cast<uint32>(out_.typeFields.Size())) ||
            !out_.typeFields.TryPushBack(item)) {
            Fail(ErrorCode::OutOfMemory);
            return NoBytecodeIndex;
        }
    }

    BytecodeType& stored = out_.types[index];
    stored.fieldCount = static_cast<uint32>(fields.Size());
    for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
        stored.slots[bank] = static_cast<uint16>(slots[bank]);
    }

    for (uint32 at = stored.firstField; at < stored.firstField + stored.fieldCount; ++at) {
        BytecodeTypeField& field = out_.typeFields[at];
        if (stored.kind == BytecodeTypeKind::Struct) {
            field.offset += field.bank == 0 ? 0 : field.bank == 1 ? slots[0] : slots[0] + slots[1];
        }
    }

    return index;
}

uint32 BytecodeEncoder::CallRef(uint32 callee, uint8 sub, uint32 types, Opcode& op) noexcept {
    const Symbol& entry = c_.symbols_[callee];
    if (sub == IrCallRpc) {
        op = Opcode::CallRpc;
        return SymbolRef(BytecodeRefKind::RpcFunction, callee, types);
    }

    switch (entry.kind) {
    case SymbolKind::EngineFunction:
        op = Opcode::CallN;
        return SymbolRef(BytecodeRefKind::NativeFunction, callee, types);
    case SymbolKind::LanguageFunction:
    case SymbolKind::LanguageField:
    case SymbolKind::PreludeValue:
        op = Opcode::CallL;
        return SymbolRef(BytecodeRefKind::LanguageFunction, callee, types);
    case SymbolKind::InterfaceMethod:
        op = Opcode::CallV;
        return SymbolRef(BytecodeRefKind::VirtualFunction, callee, types);
    case SymbolKind::Method:
        if ((entry.flags & (SymbolFlags::Open | SymbolFlags::Override)) != 0) {
            op = Opcode::CallV;
            return SymbolRef(BytecodeRefKind::VirtualFunction, callee, types);
        }

        [[fallthrough]];
    default:
        op = Opcode::Call;
        return SymbolRef(entry.module == module_ ? BytecodeRefKind::Function
                                                 : BytecodeRefKind::ImportedFunction,
                         callee, types);
    }
}

uint32 BytecodeEncoder::Bx(uint32 index) noexcept {
    // 16비트 표 번호(LOADK·LOADSTR·참조)의 한도다. 모듈 표가 65,536개를 넘으면 S1150이다.
    if (index > MaxBx) {
        TooLarge();
        return 0;
    }

    return index;
}

// ---- 할당 ----

uint32 BytecodeEncoder::TupleFirst(uint32 vreg) const noexcept {
    return vreg - f_->registers[vreg].slot;
}

uint32 BytecodeEncoder::Phys(uint32 vreg) const noexcept {
    if (vreg == NoIrRegister) {
        return 0;
    }

    return physFirst_[TupleFirst(vreg)] + f_->registers[vreg].slot;
}

uint32 BytecodeEncoder::BankOf(uint32 vreg) const noexcept {
    return BankIndex(f_->registers[vreg].bank);
}

namespace {

// 명령 하나가 읽고(use) 쓰는(def) 가상 레지스터를 slot 단위로 알린다.
template <typename Use, typename Def>
void ForEachAccess(const IrFunction& f, const IrInstruction& i, Use&& use, Def&& def) noexcept {
    auto tuple = [&](uint32 first, auto&& visit) noexcept {
        if (first == NoIrRegister || first >= f.registers.Size()) {
            return;
        }

        const uint32 head = first - f.registers[first].slot;
        for (uint32 slot = 0; slot < f.registers[head].slots; ++slot) {
            visit(head + slot);
        }
    };
    auto useTuple = [&](uint32 first) noexcept { tuple(first, use); };
    auto defTuple = [&](uint32 first) noexcept { tuple(first, def); };
    auto useOne = [&](uint32 vreg) noexcept {
        if (vreg != NoIrRegister && vreg < f.registers.Size()) {
            use(vreg);
        }
    };
    auto defOne = [&](uint32 vreg) noexcept {
        if (vreg != NoIrRegister && vreg < f.registers.Size()) {
            def(vreg);
        }
    };
    auto operands = [&](uint32 first, uint32 count, uint32 stride) noexcept {
        for (uint32 index = 0; index < count * stride; index += stride) {
            useTuple(f.operands[first + index + stride - 1]);
        }
    };
    switch (i.op) {
    case IrOp::Move:
    case IrOp::Unary:
        useOne(i.b);
        defOne(i.a);
        break;
    case IrOp::Binary:
        if (static_cast<IrArith>(i.sub) == IrArith::Compose) {
            useTuple(i.b); // quat·mat4 합성은 묶음 전체를 읽고 쓴다
            useTuple(i.c);
            defTuple(i.a);
            break;
        }

        useOne(i.b);
        useOne(i.c);
        defOne(i.a);
        break;
    case IrOp::LoadInt:
    case IrOp::LoadFloat:
    case IrOp::LoadString:
    case IrOp::LoadNull:
        defOne(i.a);
        break;
    case IrOp::Compare:
        useTuple(i.b);
        useTuple(i.c);
        defOne(i.a);
        break;
    case IrOp::Convert:
    case IrOp::Length:
        useTuple(i.b);
        defTuple(i.a);
        break;
    case IrOp::IsSome:
        useOne(i.b);
        defOne(i.a);
        break;
    case IrOp::Branch:
    case IrOp::Switch:
        useOne(i.b);
        break;
    case IrOp::Return:
    case IrOp::ReturnError:
    case IrOp::EndExecution:
    case IrOp::Fault:
        useTuple(i.b);
        break;
    case IrOp::GetField:
        useOne(i.b);
        defTuple(i.a);
        break;
    case IrOp::SetField:
        useOne(i.a);
        useTuple(i.b);
        if (f.registers[i.a].bank != IrBank::Ref) {
            defOne(i.a); // struct 값의 필드 쓰기는 값을 바꾼다
        }

        break;
    case IrOp::GetElement:
    case IrOp::GetEntry:
        useOne(i.b);
        useTuple(i.c);
        defTuple(i.a);
        break;
    case IrOp::SetElement:
        useOne(i.a);
        useTuple(i.b);
        useTuple(i.c);
        defOne(i.a);
        break;
    case IrOp::Call:
        operands(i.b, i.c, 1);
        if (i.sub == IrCallMutating && i.c > 0) {
            defTuple(f.operands[i.b]);
        }

        defTuple(i.a);
        break;
    case IrOp::Emit:
        useOne(i.a == NoIrRegister ? f.self : i.a); // 대상이 없으면 자기 객체(this)
        operands(i.b, i.c, 1);
        break;
    case IrOp::Concat:
        operands(i.b, i.c, 1);
        defOne(i.a);
        break;
    case IrOp::New:
        operands(i.b, i.c, i.sub == IrNewStruct || i.sub == IrNewLiteral ? 2 : 1);
        defTuple(i.a);
        break;
    case IrOp::Await:
        useOne(i.b);
        defTuple(i.a);
        break;
    case IrOp::Try:
        useTuple(i.b);
        defTuple(i.a);
        break;
    case IrOp::Release:
        useOne(i.a);
        break;
    case IrOp::Jump:
        break;
    }
}

template <typename Visit>
void ForEachSuccessor(const IrFunction& f, const IrInstruction& last, Visit&& visit) noexcept {
    switch (last.op) {
    case IrOp::Jump:
        visit(last.a);
        break;
    case IrOp::Branch:
        visit(last.a);
        visit(last.c);
        break;
    case IrOp::Switch:
        for (uint32 index = last.a; index < last.a + last.c; ++index) {
            visit(f.cases[index].block);
        }

        visit(last.extra);
        break;
    case IrOp::Await:
        visit(last.c);
        break;
    case IrOp::Try:
        visit(last.extra);
        visit(last.c);
        break;
    default:
        break;
    }
}

} // namespace

bool BytecodeEncoder::Allocate() noexcept {
    const IrFunction& f = *f_;
    const uint32 registers = static_cast<uint32>(f.registers.Size());
    const uint32 blocks = static_cast<uint32>(f.blocks.Size());

    // 배치: 진입 블록에서 도달하는 블록을 IR 순서로 둔다.
    Array<uint8> reached(checker_.allocator_);
    Array<uint32> pending(checker_.allocator_);
    if (!reached.TryResize(blocks) || !layoutOf_.TryResize(blocks) ||
        !firstPos_.TryResize(blocks)) {
        return Fail(ErrorCode::OutOfMemory);
    }

    if (blocks > 0 && f.blocks[0].count > 0) {
        reached[0] = 1;
        if (auto pushed = pending.TryPushBack(0); !pushed) {
            return Fail(pushed.Error());
        }
    }

    while (!pending.IsEmpty()) {
        const uint32 block = pending.TryPopBack().Value();
        const IrBlock& entry = f.blocks[block];
        bool failed = false;
        ForEachSuccessor(f, f.instructions[entry.first + entry.count - 1],
                         [&](uint32 next) noexcept {
                             if (next < blocks && reached[next] == 0) {
                                 reached[next] = 1;
                                 failed = failed || !pending.TryPushBack(next);
                             }
                         });
        if (failed) {
            return Fail(ErrorCode::OutOfMemory);
        }
    }

    order_.Clear();
    uint32 position = 0;
    for (uint32 block = 0; block < blocks; ++block) {
        layoutOf_[block] = NoIrBlock;
        if (reached[block] == 0) {
            continue;
        }

        layoutOf_[block] = static_cast<uint32>(order_.Size());
        firstPos_[block] = position;
        position += f.blocks[block].count;
        if (auto pushed = order_.TryPushBack(block); !pushed) {
            return Fail(pushed.Error());
        }
    }

    const uint32 lastPosition = position == 0 ? 0 : 2 * position - 1;

    // 블록 단위 생존 분석(가상 레지스터 slot마다 — 비트 집합).
    const uint32 words = (registers + BitsPerWord - 1) / BitsPerWord;
    const usize setSize = usize(words) * order_.Size();
    Array<uint64> uses(checker_.allocator_);
    Array<uint64> defs(checker_.allocator_);
    Array<uint64> liveIn(checker_.allocator_);
    Array<uint64> liveOut(checker_.allocator_);
    if (!uses.TryResize(setSize) || !defs.TryResize(setSize) || !liveIn.TryResize(setSize) ||
        !liveOut.TryResize(setSize)) {
        return Fail(ErrorCode::OutOfMemory);
    }

    auto bit = [](Array<uint64>& set, usize base, uint32 vreg) noexcept -> uint64& {
        return set[base + vreg / BitsPerWord];
    };
    for (usize at = 0; at < order_.Size(); ++at) {
        const IrBlock& entry = f.blocks[order_[at]];
        const usize base = at * words;
        for (uint32 index = entry.first; index < entry.first + entry.count; ++index) {
            ForEachAccess(
                f, f.instructions[index],
                [&](uint32 vreg) noexcept {
                    const uint64 mask = uint64(1) << (vreg % BitsPerWord);
                    if ((bit(defs, base, vreg) & mask) == 0) {
                        bit(uses, base, vreg) |= mask;
                    }
                },
                [&](uint32 vreg) noexcept {
                    bit(defs, base, vreg) |= uint64(1) << (vreg % BitsPerWord);
                });
        }
    }

    for (bool changed = true; changed;) {
        changed = false;
        for (usize at = order_.Size(); at > 0; --at) {
            const IrBlock& entry = f.blocks[order_[at - 1]];
            const usize base = (at - 1) * words;
            ForEachSuccessor(f, f.instructions[entry.first + entry.count - 1],
                             [&](uint32 next) noexcept {
                                 const usize other = usize(layoutOf_[next]) * words;
                                 for (uint32 word = 0; word < words; ++word) {
                                     liveOut[base + word] |= liveIn[other + word];
                                 }
                             });
            for (uint32 word = 0; word < words; ++word) {
                const uint64 in = uses[base + word] | (liveOut[base + word] & ~defs[base + word]);
                if (in != liveIn[base + word]) {
                    liveIn[base + word] = in;
                    changed = true;
                }
            }
        }
    }

    // 묶음마다 생존 구간: 블록 경계의 생존과 명령의 읽기(2k)·쓰기(2k + 1).
    if (!start_.TryResize(registers) || !end_.TryResize(registers) ||
        !physFirst_.TryResize(registers) || !stringOf_.TryResize(registers)) {
        return Fail(ErrorCode::OutOfMemory);
    }

    for (uint32 vreg = 0; vreg < registers; ++vreg) {
        start_[vreg] = NoPosition;
        end_[vreg] = 0;
        physFirst_[vreg] = 0;
        stringOf_[vreg] = NoBytecodeIndex;
    }

    auto extend = [&](uint32 vreg, uint32 at) noexcept {
        const uint32 first = TupleFirst(vreg);
        start_[first] = start_[first] == NoPosition || at < start_[first] ? at : start_[first];
        end_[first] = at > end_[first] ? at : end_[first];
    };
    for (usize at = 0; at < order_.Size(); ++at) {
        const uint32 block = order_[at];
        const IrBlock& entry = f.blocks[block];
        const uint32 begin = 2 * firstPos_[block];
        const uint32 finish = 2 * (firstPos_[block] + entry.count) - 1;
        const usize base = at * words;
        for (uint32 vreg = 0; vreg < registers; ++vreg) {
            const uint64 mask = uint64(1) << (vreg % BitsPerWord);
            if ((liveIn[base + vreg / BitsPerWord] & mask) != 0) {
                extend(vreg, begin);
            }

            if ((liveOut[base + vreg / BitsPerWord] & mask) != 0) {
                extend(vreg, finish);
            }
        }

        for (uint32 index = 0; index < entry.count; ++index) {
            const uint32 at2 = 2 * (firstPos_[block] + index);
            // 생성(NEWS 뒤 필드마다 SETSF)과 합성 곱은 결과를 먼저 쓰고 피연산자를 읽으므로
            // 결과를 읽기 위치부터 살려 둔다(early clobber — 피연산자와 같은 레지스터를 받지
            // 않는다).
            const IrInstruction& current = f.instructions[entry.first + index];
            const bool early =
                current.op == IrOp::New || (current.op == IrOp::Binary &&
                                            static_cast<IrArith>(current.sub) == IrArith::Compose);
            ForEachAccess(
                f, f.instructions[entry.first + index],
                [&](uint32 vreg) noexcept { extend(vreg, at2); },
                [&](uint32 vreg) noexcept { extend(vreg, early ? at2 : at2 + 1); });
        }
    }

    // 매개변수·this는 고정 번호이며 함수 전체에서 살아 있다.
    for (uint32& count : registers_) {
        count = 0;
    }

    Array<uint32> busy[BytecodeBankCount] = {Array<uint32>(checker_.allocator_),
                                             Array<uint32>(checker_.allocator_),
                                             Array<uint32>(checker_.allocator_)};
    for (Array<uint32>& bank : busy) {
        if (!bank.TryResize(MaxBankRegisters)) {
            return Fail(ErrorCode::OutOfMemory);
        }

        for (uint32& until : bank) {
            until = NoPosition; // 비어 있음
        }
    }

    Array<uint8> fixed(checker_.allocator_);
    if (!fixed.TryResize(registers)) {
        return Fail(ErrorCode::OutOfMemory);
    }

    auto place = [&](uint32 first, uint32 at, uint32 until) noexcept {
        const uint32 bank = BankOf(first);
        const uint32 slots = f.registers[first].slots;
        physFirst_[first] = at;
        for (uint32 slot = 0; slot < slots; ++slot) {
            busy[bank][at + slot] = until;
        }

        registers_[bank] = at + slots > registers_[bank] ? at + slots : registers_[bank];
    };
    auto placeFixed = [&](uint32 first) noexcept {
        if (first == NoIrRegister) {
            return true;
        }

        const uint32 bank = BankOf(first);
        const uint32 slots = f.registers[first].slots;
        if (registers_[bank] + slots > MaxBankRegisters) {
            return false;
        }

        fixed[first] = 1;
        place(first, registers_[bank], lastPosition + 1);
        return true;
    };
    bool fits = placeFixed(f.self);
    for (const uint32 parameter : f.parameters) {
        fits = fits && placeFixed(parameter);
    }

    BytecodeFunction& function = out_.functions[out_.functions.Size() - 1];
    for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
        function.parameters[bank] = static_cast<uint16>(registers_[bank]);
    }

    // 선형 스캔: 시작 순(같으면 번호 순)으로, 시작 전에 끝난 자리를 다시 쓴다. 묶음은 연속 자리 중
    // 가장 낮은 곳에 둔다.
    Array<uint32> tuples(checker_.allocator_);
    for (uint32 vreg = 0; vreg < registers; ++vreg) {
        if (f.registers[vreg].slot == 0 && fixed[vreg] == 0 && start_[vreg] != NoPosition) {
            if (auto pushed = tuples.TryPushBack(vreg); !pushed) {
                return Fail(pushed.Error());
            }
        }
    }

    algo::Sort(tuples.View(), [&](uint32 left, uint32 right) noexcept {
        return start_[left] != start_[right] ? start_[left] < start_[right] : left < right;
    });
    for (const uint32 first : tuples) {
        const uint32 bank = BankOf(first);
        const uint32 slots = f.registers[first].slots;
        uint32 found = MaxBankRegisters;
        for (uint32 at = 0; at + slots <= MaxBankRegisters && found == MaxBankRegisters; ++at) {
            bool free = true;
            for (uint32 slot = 0; slot < slots && free; ++slot) {
                const uint32 until = busy[bank][at + slot];
                free = until == NoPosition || until < start_[first];
            }

            found = free ? at : MaxBankRegisters;
        }

        if (found == MaxBankRegisters) {
            fits = false;
            break;
        }

        place(first, found, end_[first]);
    }

    if (!fits) {
        TooLarge();
        return false;
    }

    return true;
}

void BytecodeEncoder::TooLarge() noexcept {
    if (tooLarge_) {
        return;
    }

    tooLarge_ = true;
    CheckContext context;
    context.module = module_;
    checker_.Report(context, DiagnosticCode::FunctionTooLarge, nameNode_,
                    "The function is too large to compile.",
                    "A function uses at most 256 values of each kind at once and 32,767 "
                    "instructions.",
                    "Split the function into smaller functions.");
}

// ---- 인코딩 ----

void BytecodeEncoder::Word(uint32 word, SourceSpan span) noexcept {
    if (checker_.binder_.error_ != ErrorCode::None) {
        return;
    }

    const SourceFile& file = c_.sources_->Get(c_.modules_[module_].source);
    const SourceLocation at = file.Locate(span.begin);
    if (auto pushed = out_.words.TryPushBack(word); !pushed) {
        Fail(pushed.Error());
        return;
    }

    if (auto pushed = out_.locations.TryPushBack(BytecodeLocation{at.line, at.column}); !pushed) {
        static_cast<void>(out_.words.TryResize(out_.words.Size() - 1));
        Fail(pushed.Error());
    }
}

void BytecodeEncoder::Branch(Opcode op, uint32 a, uint32 block, SourceSpan span) noexcept {
    const uint32 address = static_cast<uint32>(out_.words.Size()) - firstWord_;
    if (auto pushed = fixups_.TryPushBack(Fixup{address, block, false}); !pushed) {
        Fail(pushed.Error());
        return;
    }

    Word(op == Opcode::Jmp ? EncodeAsBx(op, 0, 0) : EncodeAsBx(op, a, 0), span);
}

void BytecodeEncoder::JumpTo(uint32 block, uint32 layoutIndex, SourceSpan span) noexcept {
    // 다음에 놓이는 블록으로 가는 JMP는 내지 않는다.
    if (layoutIndex + 1 < order_.Size() && order_[layoutIndex + 1] == block) {
        return;
    }

    Branch(Opcode::Jmp, 0, block, span);
}

void BytecodeEncoder::Move(uint32 bank, uint32 to, uint32 from, SourceSpan span) noexcept {
    if (to == from) {
        return;
    }

    const Opcode op = bank == 0 ? Opcode::MovS : bank == 1 ? Opcode::MovR : Opcode::MovB;
    Word(EncodeABC(op, to, from, 0), span);
}

void BytecodeEncoder::ToWindow(Span<const uint32> values, SourceSpan span) noexcept {
    for (uint32& cursor : cursor_) {
        cursor = 0;
    }

    for (const uint32 value : values) {
        if (value == NoIrRegister) {
            continue;
        }

        const uint32 bank = BankOf(value);
        const uint32 slots = f_->registers[TupleFirst(value)].slots;
        const uint32 base = registers_[bank] + cursor_[bank];
        if (base + slots > MaxBankRegisters) {
            TooLarge();
            return;
        }

        for (uint32 slot = 0; slot < slots; ++slot) {
            Move(bank, base + slot, Phys(value) + slot, span);
        }

        cursor_[bank] += slots;
        window_[bank] = cursor_[bank] > window_[bank] ? cursor_[bank] : window_[bank];
    }
}

void BytecodeEncoder::FromWindow(uint32 value, uint32 offset, SourceSpan span) noexcept {
    if (value == NoIrRegister) {
        return;
    }

    const uint32 bank = BankOf(value);
    const uint32 slots = f_->registers[TupleFirst(value)].slots;
    if (registers_[bank] + offset + slots > MaxBankRegisters) {
        TooLarge();
        return;
    }

    window_[bank] = offset + slots > window_[bank] ? offset + slots : window_[bank];
    for (uint32 slot = 0; slot < slots; ++slot) {
        Move(bank, Phys(value) + slot, registers_[bank] + offset + slot, span);
    }
}

uint32 BytecodeEncoder::ContainerMarker(uint32 value, uint32 key) const noexcept {
    const uint32 valueBank = value == NoIrRegister ? 0 : BankOf(value);
    const uint32 valueSlots = value == NoIrRegister ? 0 : f_->registers[TupleFirst(value)].slots;
    const uint32 keyBank = key == NoIrRegister ? 0 : BankOf(key);
    const uint32 keySlots = key == NoIrRegister ? 0 : f_->registers[TupleFirst(key)].slots;
    return EncodeContainerMarker(valueBank, valueSlots, keyBank, keySlots);
}

void BytecodeEncoder::RecordAwait(uint32 address, uint32 position, uint32 result) noexcept {
    // 정지 뒤에도 살아 있는 묶음(이 await의 결과 제외)을 bank별 구간으로 둔다.
    BytecodeAwaitPoint point;
    point.address = address;
    point.firstLive = static_cast<uint32>(out_.liveRanges.Size());
    const uint32 resultFirst = result == NoIrRegister ? NoIrRegister : TupleFirst(result);
    for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
        for (uint32 vreg = 0; vreg < f_->registers.Size(); ++vreg) {
            if (f_->registers[vreg].slot != 0 || BankOf(vreg) != bank || vreg == resultFirst ||
                start_[vreg] == NoPosition || start_[vreg] > position ||
                end_[vreg] <= position + 1) {
                continue;
            }

            const uint16 first = static_cast<uint16>(physFirst_[vreg]);
            const uint16 count = f_->registers[vreg].slots;
            if (auto pushed = out_.liveRanges.TryPushBack(
                    BytecodeLiveRange{static_cast<BytecodeBank>(bank), first, count});
                !pushed) {
                Fail(pushed.Error());
                return;
            }
        }
    }

    // 같은 bank의 구간을 번호 순으로 모아 이어진 것을 합친다(결정적 표현).
    Span<BytecodeLiveRange> ranges(out_.liveRanges.Data() + point.firstLive,
                                   out_.liveRanges.Size() - point.firstLive);
    algo::Sort(ranges, [](const BytecodeLiveRange& left, const BytecodeLiveRange& right) noexcept {
        return left.bank != right.bank ? left.bank < right.bank : left.first < right.first;
    });
    usize kept = 0;
    for (usize index = 0; index < ranges.Size(); ++index) {
        if (kept > 0 && ranges[kept - 1].bank == ranges[index].bank &&
            ranges[kept - 1].first + ranges[kept - 1].count >= ranges[index].first) {
            const uint16 end = static_cast<uint16>(ranges[index].first + ranges[index].count);
            const uint16 last =
                static_cast<uint16>(ranges[kept - 1].first + ranges[kept - 1].count);
            ranges[kept - 1].count =
                static_cast<uint16>((end > last ? end : last) - ranges[kept - 1].first);
            continue;
        }

        ranges[kept++] = ranges[index];
    }

    static_cast<void>(out_.liveRanges.TryResize(point.firstLive + kept));
    point.liveCount = static_cast<uint32>(kept);
    if (auto pushed = out_.awaits.TryPushBack(point); !pushed) {
        Fail(pushed.Error());
    }
}

bool BytecodeEncoder::Encode(const IrFunction& function, uint32 index, StringView name,
                             uint32 nameNode) noexcept {
    f_ = &function;
    nameNode_ = nameNode;
    tooLarge_ = false;
    for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
        window_[bank] = 0;
    }

    if (out_.functions.Size() != index) {
        platform::Fatal("Sparks bytecode functions must be encoded in order");
    }

    BytecodeFunction entry;
    entry.name = AddString(name);
    if (auto pushed = out_.functions.TryPushBack(entry); !pushed) {
        return Fail(pushed.Error());
    }

    if (!Allocate() || checker_.binder_.error_ != ErrorCode::None) {
        return false;
    }

    firstWord_ = static_cast<uint32>(out_.words.Size());
    const uint32 firstSwitch = static_cast<uint32>(out_.switches.Size());
    const uint32 firstAwait = static_cast<uint32>(out_.awaits.Size());
    fixups_.Clear();
    switchFixups_.Clear();
    if (!blockAddress_.TryResize(function.blocks.Size())) {
        return Fail(ErrorCode::OutOfMemory);
    }

    for (usize at = 0;
         at < order_.Size() && !tooLarge_ && checker_.binder_.error_ == ErrorCode::None; ++at) {
        const uint32 block = order_[at];
        blockAddress_[block] = static_cast<uint32>(out_.words.Size()) - firstWord_;
        const IrBlock& entryBlock = function.blocks[block];
        for (uint32 instruction = entryBlock.first;
             instruction < entryBlock.first + entryBlock.count; ++instruction) {
            Instruction(function.instructions[instruction], static_cast<uint32>(at));
        }
    }

    if (checker_.binder_.error_ != ErrorCode::None || tooLarge_) {
        return false;
    }

    const uint32 wordCount = static_cast<uint32>(out_.words.Size()) - firstWord_;
    if (wordCount > MaxFunctionWords) {
        TooLarge();
        return false;
    }

    // 분기 거리(다음 단어 기준)와 switch 표의 주소를 채운다.
    for (const Fixup& fixup : fixups_) {
        const int32 distance =
            static_cast<int32>(blockAddress_[fixup.block]) - static_cast<int32>(fixup.word + 1);
        uint32& word = out_.words[firstWord_ + fixup.word];
        word = fixup.extra ? EncodeAx(Opcode::Extra, static_cast<uint32>(distance))
                           : EncodeAsBx(DecodeOpcode(word), DecodeA(word), distance);
    }

    for (const uint32 table : switchFixups_) {
        BytecodeSwitchTable& entry2 = out_.switches[table];
        entry2.defaultTarget = blockAddress_[entry2.defaultTarget];
        for (uint32 at = entry2.firstCase; at < entry2.firstCase + entry2.caseCount; ++at) {
            out_.cases[at].target = blockAddress_[out_.cases[at].target];
        }
    }

    BytecodeFunction& result = out_.functions[index];
    // 반환 모양: 함수 반환 타입의 bank와 slot 수(handler·void는 slot 0).
    const uint32 signature =
        function.symbol == NoSymbol ? NoSignature : c_.symbols_[function.symbol].signature;
    const uint32 returns = signature == NoSignature ? NoType : c_.signatures_[signature].returnType;
    const IrLayout layout = IrBuilder::Layout(checker_, returns);
    result.returnBank = static_cast<uint8>(BankIndex(layout.bank));
    result.returnSlots = static_cast<uint8>(layout.slots);
    result.firstWord = firstWord_;
    result.wordCount = wordCount;
    result.firstSwitch = firstSwitch;
    result.switchCount = static_cast<uint32>(out_.switches.Size()) - firstSwitch;
    result.firstAwait = firstAwait;
    result.awaitCount = static_cast<uint32>(out_.awaits.Size()) - firstAwait;
    for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
        result.registers[bank] = static_cast<uint16>(registers_[bank]);
        result.frame[bank] = static_cast<uint16>(registers_[bank] + window_[bank]);
        if (result.frame[bank] > MaxBankRegisters) {
            TooLarge();
            return false;
        }
    }

    return true;
}

void BytecodeEncoder::Instruction(const IrInstruction& i, uint32 layoutIndex) noexcept {
    const SourceSpan span = i.span;
    const IrFunction& f = *f_;
    switch (i.op) {
    case IrOp::Move:
        Move(BankOf(i.a), Phys(i.a), Phys(i.b), span);
        return;
    case IrOp::LoadInt: {
        const int64 value = static_cast<int64>((uint64(i.extra) << 32) | i.b);
        if (value >= MinImmediate && value <= MaxImmediate) {
            Word(EncodeAsBx(Opcode::LoadI, Phys(i.a), static_cast<int32>(value)), span);
        } else {
            Word(EncodeABx(Opcode::LoadK, Phys(i.a), Bx(AddScalar(static_cast<uint64>(value)))),
                 span);
        }

        return;
    }
    case IrOp::LoadFloat:
        Word(EncodeABx(Opcode::LoadK, Phys(i.a), Bx(AddScalar((uint64(i.extra) << 32) | i.b))),
             span);
        return;
    case IrOp::LoadString: {
        const uint32 text = AddDecodedString(f.strings[i.b], i.sub == IrStringPiece);
        stringOf_[i.a] = text;
        Word(EncodeABx(Opcode::LoadStr, Phys(i.a), Bx(text)), span);
        return;
    }
    case IrOp::LoadNull:
        Word(EncodeABC(BankOf(i.a) == 1 ? Opcode::LoadNullR : Opcode::LoadNullB, Phys(i.a), 0, 0),
             span);
        return;
    case IrOp::Unary: {
        const IrScalar scalar = f.registers[i.a].scalar;
        Opcode op = Opcode::Not;
        if (static_cast<IrArith>(i.sub) == IrArith::Neg) {
            op = IsFloat(scalar) ? (scalar == IrScalar::F32 ? Opcode::NegF32 : Opcode::NegF64)
                                 : Offset(Opcode::NegI8, WidthOf(scalar) & 3);
        }

        Word(EncodeABC(op, Phys(i.a), Phys(i.b), 0), span);
        return;
    }
    case IrOp::Binary: {
        if (static_cast<IrArith>(i.sub) == IrArith::Compose) {
            const bool quat = f.registers[TupleFirst(i.a)].slots == 4;
            Word(EncodeABC(quat ? Opcode::MulQ : Opcode::MulM, Phys(i.a), Phys(i.b), Phys(i.c)),
                 span);
            return;
        }

        const IrScalar scalar = f.registers[i.a].scalar;
        const uint32 arith = i.sub; // Add·Sub·Mul·Div·Mod = 0…4
        Opcode op = Opcode::AddI64;
        if (IsFloat(scalar)) {
            op = Offset(scalar == IrScalar::F32 ? Opcode::AddF32 : Opcode::AddF64, arith);
        } else {
            op = Offset(Opcode::AddI8, arith * WidthCount + WidthOf(scalar));
        }

        Word(EncodeABC(op, Phys(i.a), Phys(i.b), Phys(i.c)), span);
        return;
    }
    case IrOp::Compare:
        EncodeCompare(i);
        return;
    case IrOp::Convert:
        EncodeConvert(i);
        return;
    case IrOp::Jump:
        JumpTo(i.a, layoutIndex, span);
        return;
    case IrOp::Branch:
        // 다음 블록이 참 갈래면 BRF, 거짓 갈래면 BRT, 둘 다 아니면 BRT 뒤 JMP.
        if (layoutIndex + 1 < order_.Size() && order_[layoutIndex + 1] == i.a) {
            Branch(Opcode::Brf, Phys(i.b), i.c, span);
        } else {
            Branch(Opcode::Brt, Phys(i.b), i.a, span);
            JumpTo(i.c, layoutIndex, span);
        }

        return;
    case IrOp::Switch: {
        const uint32 table = static_cast<uint32>(out_.switches.Size());
        BytecodeSwitchTable entry;
        entry.firstCase = static_cast<uint32>(out_.cases.Size());
        entry.caseCount = i.c;
        entry.defaultTarget = i.extra; // 블록 — 끝에 주소로 바꾼다
        for (uint32 index = i.a; index < i.a + i.c; ++index) {
            if (auto pushed = out_.cases.TryPushBack(
                    BytecodeSwitchCase{f.cases[index].value, f.cases[index].block});
                !pushed) {
                Fail(pushed.Error());
                return;
            }
        }

        if (!out_.switches.TryPushBack(entry) || !switchFixups_.TryPushBack(table)) {
            Fail(ErrorCode::OutOfMemory);
            return;
        }

        Word(EncodeABx(Opcode::Switch, Phys(i.b), Bx(table)), span);
        return;
    }
    case IrOp::Return:
        if (i.b == NoIrRegister) {
            Word(EncodeABC(Opcode::Ret, 0, 0, 0), span);
        } else {
            Word(EncodeABC(Opcode::Ret, Phys(i.b), f.registers[TupleFirst(i.b)].slots, BankOf(i.b)),
                 span);
        }

        return;
    case IrOp::ReturnError:
        if (BankOf(i.b) == 0) {
            Word(EncodeABC(Opcode::RetErrS, Phys(i.b), f.registers[TupleFirst(i.b)].slots, 0),
                 span);
        } else {
            Word(EncodeABC(Opcode::RetErrB, Phys(i.b), 0, 0), span);
        }

        return;
    case IrOp::IsSome:
        if (BankOf(i.b) == 0) {
            Move(0, Phys(i.a), Phys(i.b), span); // Scalar `T?`의 있음 표지
        } else {
            Word(EncodeABC(BankOf(i.b) == 1 ? Opcode::IsSomeR : Opcode::IsSomeB, Phys(i.a),
                           Phys(i.b), 0),
                 span);
        }

        return;
    case IrOp::GetField: {
        if (i.b == NoIrRegister) {
            Word(EncodeABx(Opcode::GetCtx, Phys(i.a),
                           Bx(SymbolRef(BytecodeRefKind::Context, i.extra))),
                 span);
            return;
        }

        const bool object = BankOf(i.b) == 1;
        const Opcode op = Offset(object ? Opcode::GetFS : Opcode::GetSFS, BankOf(i.a));
        // C는 값의 slot 수다(verifier가 frame 범위를 본다 — 형식 버전 2의 TYPE 구역 전까지).
        Word(EncodeABC(op, Phys(i.a), Phys(i.b), f.registers[TupleFirst(i.a)].slots), span);
        Word(EncodeAx(Opcode::Extra,
                      SymbolRef(object ? BytecodeRefKind::Field : BytecodeRefKind::StructField,
                                i.extra)),
             span);
        return;
    }
    case IrOp::SetField: {
        const bool object = BankOf(i.a) == 1;
        const Opcode op = Offset(object ? Opcode::SetFS : Opcode::SetSFS, BankOf(i.b));
        Word(EncodeABC(op, Phys(i.a), Phys(i.b), f.registers[TupleFirst(i.b)].slots), span);
        Word(EncodeAx(Opcode::Extra,
                      SymbolRef(object ? BytecodeRefKind::Field : BytecodeRefKind::StructField,
                                i.extra)),
             span);
        return;
    }
    case IrOp::GetElement:
        Word(EncodeABC(Opcode::GetE, Phys(i.a), Phys(i.b), Phys(i.c)), span);
        Word(EncodeAx(Opcode::Extra, ContainerMarker(i.a, i.c)), span);
        return;
    case IrOp::SetElement:
        Word(EncodeABC(Opcode::SetE, Phys(i.a), Phys(i.b), Phys(i.c)), span);
        Word(EncodeAx(Opcode::Extra, ContainerMarker(i.c, i.b)), span);
        return;
    case IrOp::Length:
        Word(EncodeABC(Opcode::Len, Phys(i.a), Phys(i.b), 0), span);
        return;
    case IrOp::GetEntry:
        Word(EncodeABC(i.sub == IrEntryKey ? Opcode::GetKey : Opcode::GetVal, Phys(i.a), Phys(i.b),
                       Phys(i.c)),
             span);
        Word(EncodeAx(Opcode::Extra, ContainerMarker(i.a, NoIrRegister)), span);
        return;
    case IrOp::Call:
        EncodeCall(i);
        return;
    case IrOp::Emit: {
        // 대상(자기 객체면 this)이 Ref 창 0번, 인자는 그 뒤다.
        const uint32 target = i.a == NoIrRegister ? f.self : i.a;
        Array<uint32> values(checker_.allocator_);
        if (!values.TryPushBack(target)) {
            Fail(ErrorCode::OutOfMemory);
            return;
        }

        for (uint32 index = 0; index < i.c; ++index) {
            if (!values.TryPushBack(f.operands[i.b + index])) {
                Fail(ErrorCode::OutOfMemory);
                return;
            }
        }

        ToWindow(values.View(), span);
        Word(EncodeABx(Opcode::Emit, 0, Bx(SymbolRef(BytecodeRefKind::Event, i.extra))), span);
        return;
    }
    case IrOp::Await: {
        const uint32 address = static_cast<uint32>(out_.words.Size()) - firstWord_;
        if (i.sub == IrAwaitEvent) {
            Word(EncodeABC(Opcode::AwaitEv, Phys(i.b), 0, 0), span);
            Word(EncodeAx(Opcode::Extra, SymbolRef(BytecodeRefKind::Event, i.extra)), span);
        } else {
            const uint32 slots = i.a == NoIrRegister ? 0 : f.registers[TupleFirst(i.a)].slots;
            const uint32 bank = i.a == NoIrRegister ? 0 : BankOf(i.a);
            Word(EncodeABC(Opcode::Await, Phys(i.a), Phys(i.b), 0), span);
            Word(EncodeAx(Opcode::Extra, (bank << 8) | slots), span);
        }

        // 정지 지점의 선형 위치: 이 명령의 쓰기 위치(2k + 1)까지 살아 있는 것이 보존 대상이다.
        const uint32 block = order_[layoutIndex];
        const IrInstruction* base = f.instructions.Data() + f.blocks[block].first;
        const uint32 at = firstPos_[block] + static_cast<uint32>(&i - base);
        RecordAwait(address, 2 * at, i.a);
        JumpTo(i.c, layoutIndex, span);
        return;
    }
    case IrOp::Try: {
        // 성공이면 값을 꺼내 다음으로, 실패면 EXTRA의 거리로 실패 블록에 간다.
        const uint32 sourceSlots = f.registers[TupleFirst(i.b)].slots;
        if (BankOf(i.b) == 0) {
            Word(EncodeABC(Opcode::TryS, Phys(i.a), Phys(i.b), sourceSlots), span);
        } else {
            const uint32 valueSlots = i.a == NoIrRegister ? 0 : f.registers[TupleFirst(i.a)].slots;
            const uint32 valueBank = i.a == NoIrRegister ? 0 : BankOf(i.a);
            Word(EncodeABC(Opcode::TryB, Phys(i.a), Phys(i.b), (valueBank << 5) | valueSlots),
                 span);
        }

        const uint32 address = static_cast<uint32>(out_.words.Size()) - firstWord_;
        if (auto pushed = fixups_.TryPushBack(Fixup{address, i.c, true}); !pushed) {
            Fail(pushed.Error());
            return;
        }

        Word(EncodeAx(Opcode::Extra, 0), span);
        JumpTo(i.extra, layoutIndex, span);
        return;
    }
    case IrOp::New:
        EncodeNew(i);
        return;
    case IrOp::Concat: {
        ToWindow(Span<const uint32>(f.operands.Data() + i.b, i.c), span);
        Word(EncodeABC(Opcode::Concat, Phys(i.a), registers_[2], i.c), span);
        return;
    }
    case IrOp::Release:
        Word(EncodeABC(Opcode::Release, Phys(i.a), 0, 0), span);
        return;
    case IrOp::EndExecution:
        // 현재 실행을 끝내고 Result의 오류를 기록한다(Scalar Result는 묶음, Buf Result는 handle).
        if (BankOf(i.b) == 0) {
            Word(EncodeABC(Opcode::EndExecS, Phys(i.b), f.registers[TupleFirst(i.b)].slots, 0),
                 span);
        } else {
            Word(EncodeABC(Opcode::EndExecB, Phys(i.b), 0, 0), span);
        }

        return;
    case IrOp::Fault:
        Word(EncodeAx(Opcode::Fault, i.extra), span);
        return;
    }
}

void BytecodeEncoder::EncodeCompare(const IrInstruction& i) noexcept {
    const IrFunction& f = *f_;
    const SourceSpan span = i.span;
    IrCompare kind = static_cast<IrCompare>(i.sub);
    uint32 left = Phys(i.b);
    uint32 right = Phys(i.c);
    // `>`·`>=`는 피연산자를 바꾼 `<`·`<=`다.
    if (kind == IrCompare::Greater || kind == IrCompare::GreaterEqual) {
        const uint32 swap = left;
        left = right;
        right = swap;
        kind = kind == IrCompare::Greater ? IrCompare::Less : IrCompare::LessEqual;
    }

    const bool equality = kind == IrCompare::Equal || kind == IrCompare::NotEqual;
    const bool equal = kind == IrCompare::Equal;
    if (i.extra > 1) {
        if (!equality) {
            platform::Fatal("Sparks bytecode: ordering on a value tuple");
        }

        // 성분이 모두 실수(와 있음 표지)인 묶음은 실수 비교(-0.0 == 0.0), 그 밖(Soft의 AssetId
        // 등)은 비트 비교다.
        bool floats = true;
        const uint32 first = TupleFirst(i.b);
        for (uint32 slot = 0; slot < i.extra && floats; ++slot) {
            const IrScalar scalar = f.registers[first + slot].scalar;
            floats = IsFloat(scalar) || scalar == IrScalar::Flag;
        }

        const Opcode op =
            floats ? (equal ? Opcode::EqNF : Opcode::NeNF) : (equal ? Opcode::EqN : Opcode::NeN);
        Word(EncodeABC(op, Phys(i.a), left, right), span);
        Word(EncodeAx(Opcode::Extra, i.extra), span);
        return;
    }

    const uint32 bank = BankOf(i.b);
    Opcode op = Opcode::EqI;
    if (bank == 1) {
        if (!equality) {
            platform::Fatal("Sparks bytecode: ordering on object handles");
        }

        op = equal ? Opcode::EqR : Opcode::NeR;
    } else if (bank == 2) {
        // 문자열(`string?` 포함)은 UTF-8 byte 순 비교, 그 밖의 Buf는 값 비교(같음만)다.
        const uint32 type = f.registers[i.b].type;
        const bool text = type != NoType && checker_.NonOptional(type) == checker_.stringType_;
        if (!equality && !text) {
            platform::Fatal("Sparks bytecode: ordering on buffers");
        }

        op = text ? (kind == IrCompare::Equal      ? Opcode::EqStr
                     : kind == IrCompare::NotEqual ? Opcode::NeStr
                     : kind == IrCompare::Less     ? Opcode::LtStr
                                                   : Opcode::LeStr)
                  : (equal ? Opcode::EqB : Opcode::NeB);
    } else {
        const IrScalar scalar = f.registers[i.b].scalar;
        if (IsFloat(scalar)) {
            op = kind == IrCompare::Equal      ? Opcode::EqF
                 : kind == IrCompare::NotEqual ? Opcode::NeF
                 : kind == IrCompare::Less     ? Opcode::LtF
                                               : Opcode::LeF;
        } else if (!equality && IsUnsigned(scalar)) {
            op = kind == IrCompare::Less ? Opcode::LtU : Opcode::LeU;
        } else {
            op = kind == IrCompare::Equal      ? Opcode::EqI
                 : kind == IrCompare::NotEqual ? Opcode::NeI
                 : kind == IrCompare::Less     ? Opcode::LtI
                                               : Opcode::LeI;
        }
    }

    Word(EncodeABC(op, Phys(i.a), left, right), span);
}

void BytecodeEncoder::EncodeConvert(const IrInstruction& i) noexcept {
    const IrFunction& f = *f_;
    const SourceSpan span = i.span;
    if (i.sub == IrConvertCast) {
        Word(EncodeABC(Opcode::Cast, Phys(i.a), Phys(i.b), 0), span);
        Word(EncodeAx(Opcode::Extra, TypeRef(checker_.TypeOfSymbol(i.extra))), span);
        return;
    }

    const uint32 targetBank = BankOf(i.a);
    const uint32 sourceBank = BankOf(i.b);
    if (targetBank == 2 && sourceBank == 2) {
        Move(2, Phys(i.a), Phys(i.b), span); // 문자열(Text 포함)은 그대로
        return;
    }

    if (targetBank == 2) {
        // 보간·`string(x)`: Scalar 묶음의 문자열. 수학 타입은 표시 종류, enum은 타입 이름 표가
        // 필요하다.
        const uint32 type = f.registers[i.b].type;
        const uint32 symbol = checker_.SymbolOf(checker_.NonOptional(type));
        const StringView name = symbol == NoSymbol ? StringView() : c_.symbols_[symbol].name;
        uint32 display = static_cast<uint32>(KindOf(f.registers[i.b].scalar));
        if (name == "vec2") {
            display = static_cast<uint32>(DisplayKind::Vec2);
        } else if (name == "vec3") {
            display = static_cast<uint32>(DisplayKind::Vec3);
        } else if (name == "vec4") {
            display = static_cast<uint32>(DisplayKind::Vec4);
        } else if (name == "quat") {
            display = static_cast<uint32>(DisplayKind::Quat);
        } else if (name == "Rotator") {
            display = static_cast<uint32>(DisplayKind::Rotator);
        } else if (name == "Color") {
            display = static_cast<uint32>(DisplayKind::Color);
        } else if (name == "Transform") {
            display = static_cast<uint32>(DisplayKind::Transform);
        } else if (name == "mat4") {
            display = static_cast<uint32>(DisplayKind::Mat4);
        }

        const uint32 enumType =
            f.registers[i.b].scalar == IrScalar::Enum ? TypeRef(checker_.NonOptional(type)) : 0;
        Word(EncodeABC(Opcode::ToStr, Phys(i.a), Phys(i.b), f.registers[TupleFirst(i.b)].slots),
             span);
        Word(EncodeAx(Opcode::Extra, (display << 16) | (Bx(enumType) & MaxBx)), span);
        return;
    }

    const ScalarKind from = KindOf(f.registers[i.b].scalar);
    const ScalarKind to = KindOf(f.registers[i.a].scalar);
    if (from == to) {
        Move(0, Phys(i.a), Phys(i.b), span);
        return;
    }

    Word(EncodeABC(Opcode::Conv, Phys(i.a), Phys(i.b),
                   (static_cast<uint32>(from) << 4) | static_cast<uint32>(to)),
         span);
}

void BytecodeEncoder::EncodeCall(const IrInstruction& i) noexcept {
    // 인자(수신자 먼저)를 창에 두고 부른 뒤, 결과를 창 base에서 받는다. 값을 바꾸는 메서드는
    // 수신자를 창 0번에서 돌려받고, 같은 bank의 결과는 그 다음 자리에서 받는다.
    const IrFunction& f = *f_;
    const SourceSpan span = i.span;
    Opcode op = Opcode::Call;
    const SymbolKind calleeKind = c_.symbols_[i.extra].kind;
    const bool language = i.sub != IrCallRpc && (calleeKind == SymbolKind::LanguageFunction ||
                                                 calleeKind == SymbolKind::LanguageField ||
                                                 calleeKind == SymbolKind::PreludeValue);
    uint32 ref = language ? 0 : Bx(CallRef(i.extra, i.sub, i.types, op));
    if (language) {
        op = Opcode::CallL;
        // 언어 함수: 피연산자(수신자 먼저)와 결과의 서술자를 리터럴 인자 자리에 싣는다(SparksVm
        // 6a절 — 오버로드·원소 타입을 VM이 이것으로 고른다).
        StringBuilder<128> descriptor(checker_.allocator_);
        // 대기 handle(타입이 없는 Ref 값 — `NextTick()`의 결과, `All`의 인자)은 `w`다.
        auto operand = [&](uint32 vreg) noexcept {
            if (vreg != NoIrRegister && f.registers[vreg].type == NoType &&
                f.registers[vreg].bank == IrBank::Ref) {
                descriptor.Append("w");
            } else {
                AppendDescriptor(descriptor,
                                 vreg == NoIrRegister ? NoType : f.registers[vreg].type);
            }
        };
        for (uint32 index = 0; index < i.c; ++index) {
            descriptor.Append(index == 0 ? "" : ",");
            operand(f.operands[i.b + index]);
        }

        descriptor.Append(">");
        operand(i.a);
        if (descriptor.HasFailed()) {
            Fail(descriptor.Status().Error());
            return;
        }

        ref = Bx(SymbolRef(BytecodeRefKind::LanguageFunction, i.extra, i.types,
                           AddString(descriptor.View())));
    }

    ToWindow(Span<const uint32>(f.operands.Data() + i.b, i.c), span);
    const bool mutating = i.sub == IrCallMutating && i.c > 0;
    const uint32 receiver = mutating ? f.operands[i.b] : NoIrRegister;
    // 값을 바꾸는 메서드의 Buf 수신자는 창으로 옮긴다(원래 레지스터를 비운다): 참조가 하나뿐이어야
    // COW가 복제하지 않는다(SparksBytecode 4.3절 — 비우지 않으면 호출마다 전체 복제).
    const bool movesReceiver =
        mutating && BankOf(receiver) == static_cast<uint32>(BytecodeBank::Buf);
    if (movesReceiver) {
        Word(EncodeABC(Opcode::LoadNullB, Phys(receiver), 0, 0), span);
    }

    Word(EncodeABx(op, 0, ref), span);
    uint32 offset = 0;
    if (mutating && i.a != NoIrRegister && BankOf(i.a) == BankOf(receiver)) {
        offset = f.registers[TupleFirst(receiver)].slots;
    }

    FromWindow(i.a, offset, span);
    if (mutating) {
        FromWindow(receiver, 0, span);
    }

    if (movesReceiver) {
        Word(EncodeABC(Opcode::LoadNullB, registers_[static_cast<uint32>(BytecodeBank::Buf)], 0, 0),
             span);
    }
}

void BytecodeEncoder::EncodeNew(const IrInstruction& i) noexcept {
    const IrFunction& f = *f_;
    const SourceSpan span = i.span;
    const uint32 type = f.registers[i.a].type;
    switch (i.sub) {
    case IrNewStruct: {
        // 결과가 Ref면 객체(Sparks class·엔진 component — NEWC), 아니면 값 struct(NEWS — Sparks
        // struct는 TYPE이 있고 엔진 struct·TagSet은 host 배치라 없다, LC5d).
        const bool component = BankOf(i.a) == 1;
        // 언어 타입 `TagSet()`(빈 집합)은 null 버퍼다(SparksVm 6.5절 — VM이 이름으로 가리지 않게
        // 컴파일러가 LOADNULL.B로 낸다, LC5d2 리뷰 S5).
        const uint32 created = checker_.SymbolOf(checker_.NonOptional(type));
        if (!component && created != NoSymbol &&
            c_.symbols_[created].kind == SymbolKind::PreludeType &&
            c_.symbols_[created].name == "TagSet") {
            Word(EncodeABC(Opcode::LoadNullB, Phys(i.a), 0, 0), span);
            break;
        }

        Word(EncodeABx(component ? Opcode::NewC : Opcode::NewS, Phys(i.a), Bx(TypeRef(type))),
             span);
        for (uint32 pair = 0; pair < i.c; ++pair) {
            const uint32 field = f.operands[i.b + pair * 2];
            const uint32 value = f.operands[i.b + pair * 2 + 1];
            if (value == NoIrRegister) {
                continue;
            }

            const Opcode op = Offset(component ? Opcode::SetFS : Opcode::SetSFS, BankOf(value));
            Word(EncodeABC(op, Phys(i.a), Phys(value), f.registers[TupleFirst(value)].slots), span);
            Word(EncodeAx(Opcode::Extra, SymbolRef(component ? BytecodeRefKind::Field
                                                             : BytecodeRefKind::StructField,
                                                   field)),
                 span);
        }

        return;
    }
    case IrNewLiteral: {
        // 첫 인자(이름 글자)는 리터럴 표의 글자로, 나머지(Loc의 이름 지정 인자)는 창에 둔다. 리터럴
        // 참조의 이름은 결과 타입(`Tag`·`Soft<DataAsset>`·하드 참조 `DataAsset` 등)이고, 인자 표는
        // `이름=bank 글자;…`(창에 놓는 순서)다.
        const uint32 first = i.c > 0 ? f.operands[i.b + 1] : NoIrRegister;
        const uint32 text = first == NoIrRegister ? NoBytecodeIndex : stringOf_[first];
        StringBuilder<128> name(checker_.allocator_);
        Qualify(name, checker_.SymbolOf(checker_.NonOptional(type)));
        checker_.AppendDisplayType(name, type);
        StringBuilder<128> arguments(checker_.allocator_);
        Array<uint32> rest(checker_.allocator_);
        for (uint32 pair = 1; pair < i.c; ++pair) {
            const uint32 label = f.operands[i.b + pair * 2];
            const uint32 value = f.operands[i.b + pair * 2 + 1];
            if (value == NoIrRegister) {
                continue;
            }

            if (!rest.TryPushBack(value)) {
                Fail(ErrorCode::OutOfMemory);
                return;
            }

            const char letter = BankOf(value) == 0 ? 's' : BankOf(value) == 1 ? 'r' : 'b';
            arguments.AppendFormat("{}{}={}", rest.Size() == 1 ? "" : ";",
                                   label == NoSymbol ? StringView() : f.strings[label], letter);
        }

        if (name.HasFailed() || arguments.HasFailed()) {
            Fail(ErrorCode::OutOfMemory);
            return;
        }

        const uint32 literal = Bx(AddRef(BytecodeRefKind::Literal, AddString(name.View()), text));
        if (!rest.IsEmpty() && literal < out_.refs.Size()) {
            out_.refs[literal].arguments = AddString(arguments.View());
        }

        ToWindow(rest.View(), span);
        Word(EncodeABC(Opcode::NewLit, Phys(i.a), 0, 0), span);
        Word(EncodeAx(Opcode::Extra, (literal & MaxBx) | (static_cast<uint32>(rest.Size()) << 16) |
                                         (BankOf(i.a) << 22)),
             span);
        return;
    }
    case IrNewCollection: {
        const bool map = checker_.IsLanguage(checker_.NonOptional(type), "Map");
        const bool set = checker_.IsLanguage(checker_.NonOptional(type), "Set");
        const Opcode op = map ? Opcode::NewM : set ? Opcode::NewSet : Opcode::NewA;
        Word(EncodeABx(op, Phys(i.a), Bx(TypeRef(type))), span);
        if (map) {
            for (uint32 pair = 0; pair + 1 < i.c; pair += 2) {
                const uint32 key = f.operands[i.b + pair];
                const uint32 value = f.operands[i.b + pair + 1];
                Word(EncodeABC(Opcode::Put, Phys(i.a), Phys(key), Phys(value)), span);
                Word(EncodeAx(Opcode::Extra, ContainerMarker(value, key)), span);
            }

            return;
        }

        for (uint32 index = 0; index < i.c; ++index) {
            const uint32 value = f.operands[i.b + index];
            Word(EncodeABC(Opcode::Append, Phys(i.a), Phys(value), 0), span);
            Word(EncodeAx(Opcode::Extra, ContainerMarker(value, NoIrRegister)), span);
        }

        return;
    }
    default: {
        const uint32 value = i.c > 0 ? f.operands[i.b] : NoIrRegister;
        if (i.sub == IrNewResultError) {
            Word(EncodeABC(Opcode::Err, Phys(i.a), Phys(value), 0), span);
            return;
        }

        Word(EncodeABC(Opcode::Ok, Phys(i.a), Phys(value), 0), span);
        Word(EncodeAx(Opcode::Extra,
                      value == NoIrRegister
                          ? 0
                          : (BankOf(value) << 8) | f.registers[TupleFirst(value)].slots),
             span);
        return;
    }
    }
}

} // namespace rocks::sparks::detail
