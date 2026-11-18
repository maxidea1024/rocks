#include <Platform/Memory.h>
#include <ShaderFormatVulkan/SpirvRewrite.h>

namespace rocks::shader {

namespace {

// SPIR-V 머리 크기와 magic이다(SPIR-V 사양 2.3절).
constexpr usize HeaderWords = 5;
constexpr uint32 SpirvMagic = 0x07230203u;
constexpr usize BoundWord = 3;

// 쓰는 opcode·장식·storage class 값이다(SPIR-V 사양 3절 표, spirv.h와 같은 값).
constexpr uint32 OpName = 5;
constexpr uint32 OpExtension = 10;
constexpr uint32 OpEntryPoint = 15;
constexpr uint32 OpTypeImage = 25;
constexpr uint32 OpTypeSampler = 26;
constexpr uint32 OpTypeSampledImage = 27;
constexpr uint32 OpTypeArray = 28;
constexpr uint32 OpTypeRuntimeArray = 29;
constexpr uint32 OpTypeStruct = 30;
constexpr uint32 OpTypePointer = 32;
constexpr uint32 OpVariable = 59;
constexpr uint32 OpAccessChain = 65;
constexpr uint32 OpInBoundsAccessChain = 66;
constexpr uint32 OpPtrAccessChain = 67;
constexpr uint32 OpInBoundsPtrAccessChain = 70;
constexpr uint32 OpDecorate = 71;
constexpr uint32 OpDecorateId = 332;
constexpr uint32 OpDecorateString = 5632;
constexpr uint32 OpMemberDecorateString = 5633;

constexpr uint32 DecorationBlock = 2;
constexpr uint32 DecorationBufferBlock = 3;
constexpr uint32 DecorationBuiltIn = 11;
constexpr uint32 DecorationLocation = 30;
constexpr uint32 DecorationBinding = 33;
constexpr uint32 DecorationDescriptorSet = 34;
constexpr uint32 DecorationCounterBuffer = 5634;
constexpr uint32 DecorationUserSemantic = 5635;
constexpr uint32 DecorationUserType = 5636;

constexpr uint32 StorageUniformConstant = 0;
constexpr uint32 StorageInput = 1;
constexpr uint32 StorageUniform = 2;
constexpr uint32 StoragePushConstant = 9;
constexpr uint32 StorageStorageBuffer = 12;

constexpr uint32 ExecutionModelVertex = 0;
constexpr uint32 ExecutionModelFragment = 4;
constexpr uint32 ExecutionModelGLCompute = 5;

// OpTypeImage의 Sampled 피연산자 위치와 storage image 값이다.
constexpr usize ImageSampledWord = 7;
constexpr uint32 ImageSampledStorage = 2;

// 정점 입력 location 상한(ATTRIBUTE0–15)이다(Graphics MaxVertexAttributes와 같다).
constexpr uint32 MaxAttributeLocations = 16;

// 지울 확장 이름이다(-fspv-reflect가 더한다).
constexpr const char* ReflectionExtensions[] = {"SPV_GOOGLE_hlsl_functionality1",
                                                "SPV_GOOGLE_user_type"};

struct Instruction {
    usize offset = 0;
    uint32 count = 0;
    uint32 opcode = 0;
};

struct Decoration {
    uint32 target = 0;
    uint32 kind = 0;
    // 첫 literal의 word 위치다(입력 기준).
    usize literal = 0;
    usize instruction = 0;
};

struct Pointer {
    uint32 id = 0;
    uint32 storage = 0;
    uint32 pointee = 0;
    usize instruction = 0;
};

struct Variable {
    uint32 id = 0;
    uint32 type = 0;
    uint32 storage = 0;
    // 결과 타입과 storage class 피연산자의 word 위치다.
    usize typeWord = 0;
    usize storageWord = 0;
};

struct Access {
    uint32 id = 0;
    uint32 base = 0;
    usize typeWord = 0;
};

struct Named {
    uint32 id = 0;
    // 문자열이 시작하는 word 위치와 그 명령의 끝이다.
    usize text = 0;
    usize end = 0;
};

struct Insertion {
    usize after = 0;
    uint32 words[4] = {};
};

struct Resource {
    uint32 id = 0;
    graphics::ShaderBindingKind kind = graphics::ShaderBindingKind::ConstantBuffer;
    uint32 set = 0;
    uint32 binding = 0;
};

// 진단 문구를 이어 붙인다(실패해도 진단만 잘린다).
void Append(String& text, StringView part) noexcept {
    auto appended = text.TryAppend(part);
    (void)appended;
}

void AppendNumber(String& text, uint32 value) noexcept {
    char digits[12] = {};
    usize count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    char ordered[12] = {};
    for (usize index = 0; index < count; ++index) {
        ordered[index] = digits[count - 1 - index];
    }

    Append(text, StringView(ordered, count));
}

Result<void> Reject(String& diagnostics, StringView message) noexcept {
    Append(diagnostics, message);
    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

// 명령 안 literal 문자열(NUL 종료, little-endian 묶음)이다. 끝을 넘으면 빈 값이다.
StringView LiteralString(Span<const uint32> words, usize begin, usize end) noexcept {
    const usize capacity = (end - begin) * 4;
    const char* bytes = reinterpret_cast<const char*>(words.Data() + begin);
    usize length = 0;
    while (length < capacity && bytes[length] != '\0') {
        ++length;
    }

    return length < capacity ? StringView(bytes, length) : StringView();
}

bool EqualsIgnoringCase(StringView left, const char* right) noexcept {
    const usize length = platform::CStringLength(right);
    if (left.ByteCount() != length) {
        return false;
    }

    for (usize index = 0; index < length; ++index) {
        char letter = left.Data()[index];
        if (letter >= 'a' && letter <= 'z') {
            letter = static_cast<char>(letter - 'a' + 'A');
        }

        if (letter != right[index]) {
            return false;
        }
    }

    return true;
}

// "ATTRIBUTEn"(대소문자 무시)의 n이다. 형식이 아니면 MaxAttributeLocations다.
uint32 AttributeLocation(StringView semantic) noexcept {
    constexpr char Prefix[] = "ATTRIBUTE";
    constexpr usize PrefixBytes = sizeof(Prefix) - 1;
    if (semantic.ByteCount() <= PrefixBytes || semantic.ByteCount() > PrefixBytes + 2 ||
        !EqualsIgnoringCase(StringView(semantic.Data(), PrefixBytes), Prefix)) {
        return MaxAttributeLocations;
    }

    uint32 value = 0;
    for (usize index = PrefixBytes; index < semantic.ByteCount(); ++index) {
        const char digit = semantic.Data()[index];
        if (digit < '0' || digit > '9') {
            return MaxAttributeLocations;
        }

        value = value * 10 + static_cast<uint32>(digit - '0');
    }

    return value < MaxAttributeLocations ? value : MaxAttributeLocations;
}

class Rewriter {
public:
    Rewriter(Allocator& allocator, Span<const uint32> input, String& diagnostics) noexcept
        : input_(input), diagnostics_(diagnostics), instructions_(allocator),
          decorations_(allocator), pointers_(allocator), variables_(allocator),
          accesses_(allocator), names_(allocator), semantics_(allocator), types_(allocator),
          removed_(allocator), insertions_(allocator), resources_(allocator), pushIds_(allocator),
          words_(allocator) {
    }

    Result<void> Run(graphics::ShaderStage stage, StringView rootName,
                     Array<uint32>& output) noexcept {
        auto parsed = Parse();
        if (!parsed) {
            return parsed;
        }

        auto staged = CheckStage(stage);
        if (!staged) {
            return staged;
        }

        auto copied = words_.TryAppend(input_);
        if (!copied) {
            return copied;
        }

        bound_ = input_[BoundWord];
        RemoveReflectionData();
        if (stage == graphics::ShaderStage::Vertex) {
            auto located = RewriteInputLocations();
            if (!located) {
                return located;
            }
        }

        auto pushed = RewriteRootConstants(rootName);
        if (!pushed) {
            return pushed;
        }

        auto bound = RenumberResources(stage);
        if (!bound) {
            return bound;
        }

        return Emit(output);
    }

private:
    Result<void> Parse() noexcept {
        if (input_.Size() < HeaderWords || input_[0] != SpirvMagic) {
            return Reject(diagnostics_, StringView("input is not a SPIR-V module"));
        }

        usize offset = HeaderWords;
        while (offset < input_.Size()) {
            const uint32 first = input_[offset];
            const uint32 count = first >> 16;
            const uint32 opcode = first & 0xFFFFu;
            if (count == 0 || count > input_.Size() - offset) {
                return Reject(diagnostics_, StringView("SPIR-V instruction stream is truncated"));
            }

            const usize index = instructions_.Size();
            auto pushed = instructions_.TryPushBack(Instruction{offset, count, opcode});
            if (!pushed) {
                return pushed;
            }

            auto recorded = Record(index, offset, count, opcode);
            if (!recorded) {
                return recorded;
            }

            offset += count;
        }

        return removed_.TryResize(instructions_.Size());
    }

    Result<void> Record(usize index, usize offset, uint32 count, uint32 opcode) noexcept {
        const usize end = offset + count;
        switch (opcode) {
        case OpName:
            if (count >= 3) {
                return names_.TryPushBack(Named{input_[offset + 1], offset + 2, end});
            }

            break;
        case OpEntryPoint:
            if (count >= 2 && !hasEntryPoint_) {
                hasEntryPoint_ = true;
                executionModel_ = input_[offset + 1];
            }

            break;
        case OpTypeImage:
        case OpTypeSampler:
        case OpTypeSampledImage:
        case OpTypeArray:
        case OpTypeRuntimeArray:
        case OpTypeStruct:
            if (count >= 2) {
                return types_.TryPushBack(index);
            }

            break;
        case OpTypePointer:
            if (count == 4) {
                return pointers_.TryPushBack(
                    Pointer{input_[offset + 1], input_[offset + 2], input_[offset + 3], index});
            }

            break;
        case OpVariable:
            if (count >= 4) {
                return variables_.TryPushBack(Variable{input_[offset + 2], input_[offset + 1],
                                                       input_[offset + 3], offset + 1, offset + 3});
            }

            break;
        case OpAccessChain:
        case OpInBoundsAccessChain:
        case OpPtrAccessChain:
        case OpInBoundsPtrAccessChain:
            if (count >= 4) {
                return accesses_.TryPushBack(
                    Access{input_[offset + 2], input_[offset + 3], offset + 1});
            }

            break;
        case OpDecorate:
            if (count >= 3) {
                return decorations_.TryPushBack(
                    Decoration{input_[offset + 1], input_[offset + 2], offset + 3, index});
            }

            break;
        case OpDecorateString:
            if (count >= 4 && input_[offset + 2] == DecorationUserSemantic) {
                return semantics_.TryPushBack(Named{input_[offset + 1], offset + 3, end});
            }

            break;
        default:
            break;
        }

        return Result<void>::Success();
    }

    Result<void> CheckStage(graphics::ShaderStage stage) noexcept {
        const uint32 expected =
            stage == graphics::ShaderStage::Vertex
                ? ExecutionModelVertex
                : (stage == graphics::ShaderStage::Compute ? ExecutionModelGLCompute
                                                           : ExecutionModelFragment);
        if (!hasEntryPoint_ || executionModel_ != expected) {
            return Reject(diagnostics_,
                          StringView("SPIR-V entry point does not match the shader stage"));
        }

        return Result<void>::Success();
    }

    // -fspv-reflect가 더한 확장 선언과 장식을 지울 명령으로 표시한다.
    void RemoveReflectionData() noexcept {
        for (usize index = 0; index < instructions_.Size(); ++index) {
            const Instruction& instruction = instructions_[index];
            const usize offset = instruction.offset;
            const usize end = offset + instruction.count;
            if (instruction.opcode == OpExtension && instruction.count >= 2) {
                const StringView name = LiteralString(input_, offset + 1, end);
                for (const char* extension : ReflectionExtensions) {
                    if (name == StringView(extension)) {
                        removed_[index] = true;
                    }
                }
            } else if (instruction.opcode == OpDecorateString && instruction.count >= 3) {
                const uint32 kind = input_[offset + 2];
                removed_[index] = kind == DecorationUserSemantic || kind == DecorationUserType;
            } else if (instruction.opcode == OpMemberDecorateString && instruction.count >= 4) {
                const uint32 kind = input_[offset + 3];
                removed_[index] = kind == DecorationUserSemantic || kind == DecorationUserType;
            } else if (instruction.opcode == OpDecorateId && instruction.count >= 3) {
                removed_[index] = input_[offset + 2] == DecorationCounterBuffer;
            }
        }
    }

    const Decoration* FindDecoration(uint32 target, uint32 kind) const noexcept {
        for (const Decoration& decoration : decorations_) {
            if (decoration.target == target && decoration.kind == kind &&
                !removed_[decoration.instruction]) {
                return &decoration;
            }
        }

        return nullptr;
    }

    StringView FindString(const Array<Named>& table, uint32 id) const noexcept {
        for (const Named& entry : table) {
            if (entry.id == id) {
                return LiteralString(input_, entry.text, entry.end);
            }
        }

        return StringView();
    }

    const Pointer* FindPointer(uint32 id) const noexcept {
        for (const Pointer& pointer : pointers_) {
            if (pointer.id == id) {
                return &pointer;
            }
        }

        return nullptr;
    }

    // id의 타입 선언 명령이다(이미지·샘플러·배열·구조체만 기록한다). 없으면 null이다.
    const Instruction* FindType(uint32 id) const noexcept {
        for (usize index : types_) {
            const Instruction& instruction = instructions_[index];
            if (input_[instruction.offset + 1] == id) {
                return &instruction;
            }
        }

        return nullptr;
    }

    // 정점 입력의 location을 시맨틱 ATTRIBUTEn의 n으로 바꾼다(ShaderSystem 7.2절 7항).
    Result<void> RewriteInputLocations() noexcept {
        uint32 used = 0;
        for (const Variable& variable : variables_) {
            if (variable.storage != StorageInput ||
                FindDecoration(variable.id, DecorationBuiltIn) != nullptr) {
                continue;
            }

            const StringView semantic = FindString(semantics_, variable.id);
            const uint32 location = AttributeLocation(semantic);
            if (location >= MaxAttributeLocations) {
                Append(diagnostics_, StringView("vertex shader input "));
                Append(diagnostics_, semantic.ByteCount() != 0 ? semantic : StringView("?"));
                return Reject(diagnostics_, StringView(" must use semantic ATTRIBUTE0-15"));
            }

            if ((used & (1u << location)) != 0) {
                Append(diagnostics_, StringView("vertex shader reads location "));
                AppendNumber(diagnostics_, location);
                return Reject(diagnostics_, StringView(" twice"));
            }

            used |= 1u << location;
            const Decoration* decoration = FindDecoration(variable.id, DecorationLocation);
            if (decoration == nullptr) {
                return Reject(diagnostics_,
                              StringView("vertex shader input has no Location decoration"));
            }

            words_[decoration->literal] = location;
        }

        return Result<void>::Success();
    }

    // pointee를 가리키는 PushConstant 포인터 타입 id다. 없으면 anchor 명령 뒤에 새로 만든다.
    Result<uint32> PushPointer(uint32 pointee, usize anchor) noexcept {
        for (const Pointer& pointer : pointers_) {
            if (pointer.storage == StoragePushConstant && pointer.pointee == pointee) {
                return Result<uint32>::Success(pointer.id);
            }
        }

        const uint32 id = bound_++;
        Insertion insertion;
        insertion.after = anchor;
        insertion.words[0] = (4u << 16) | OpTypePointer;
        insertion.words[1] = id;
        insertion.words[2] = StoragePushConstant;
        insertion.words[3] = pointee;
        auto inserted = insertions_.TryPushBack(insertion);
        if (!inserted) {
            return Result<uint32>::Failure(inserted.Error());
        }

        auto recorded = pointers_.TryPushBack(Pointer{id, StoragePushConstant, pointee, anchor});
        if (!recorded) {
            return Result<uint32>::Failure(recorded.Error());
        }

        return Result<uint32>::Success(id);
    }

    // 그리기 상수 블록을 push constant로 바꾼다(ShaderSystem 7.2절 2항의 SH2 전 대역).
    Result<void> RewriteRootConstants(StringView rootName) noexcept {
        const Variable* root = nullptr;
        for (const Variable& variable : variables_) {
            if (variable.storage == StorageUniform && FindString(names_, variable.id) == rootName) {
                root = &variable;
            }
        }

        if (root == nullptr) {
            return Result<void>::Success();
        }

        const Pointer* type = FindPointer(root->type);
        if (type == nullptr || FindDecoration(type->pointee, DecorationBlock) == nullptr) {
            return Reject(diagnostics_,
                          StringView("draw constants block is not a Block constant buffer"));
        }

        rootId_ = root->id;
        const usize rootTypeWord = root->typeWord;
        const usize rootStorageWord = root->storageWord;
        const uint32 rootPointee = type->pointee;
        const usize rootAnchor = type->instruction;
        auto pointer = PushPointer(rootPointee, rootAnchor);
        if (!pointer) {
            return Result<void>::Failure(pointer.Error());
        }

        words_[rootTypeWord] = pointer.Value();
        words_[rootStorageWord] = StoragePushConstant;
        for (const Decoration& decoration : decorations_) {
            if (decoration.target == rootId_ && (decoration.kind == DecorationBinding ||
                                                 decoration.kind == DecorationDescriptorSet)) {
                removed_[decoration.instruction] = true;
            }
        }

        auto tracked = pushIds_.TryPushBack(rootId_);
        if (!tracked) {
            return tracked;
        }

        // access chain은 정의가 쓰임보다 앞서므로 한 번 훑으면 사슬 전체가 바뀐다.
        for (usize index = 0; index < accesses_.Size(); ++index) {
            const Access access = accesses_[index];
            if (!pushIds_.Contains(access.base)) {
                continue;
            }

            const Pointer* result = FindPointer(input_[access.typeWord]);
            if (result == nullptr || result->storage != StorageUniform) {
                return Reject(diagnostics_,
                              StringView("draw constants access does not yield a Uniform pointer"));
            }

            const uint32 pointee = result->pointee;
            const usize anchor = result->instruction;
            auto replacement = PushPointer(pointee, anchor);
            if (!replacement) {
                return Result<void>::Failure(replacement.Error());
            }

            words_[access.typeWord] = replacement.Value();
            auto added = pushIds_.TryPushBack(access.id);
            if (!added) {
                return added;
            }
        }

        return Result<void>::Success();
    }

    // 변수 타입이 가리키는 자원 종류다. 모르면 false다.
    bool Classify(const Variable& variable, graphics::ShaderBindingKind& kind) const noexcept {
        if (variable.storage == StorageStorageBuffer) {
            kind = graphics::ShaderBindingKind::StorageBuffer;
            return true;
        }

        const Pointer* pointer = FindPointer(variable.type);
        if (pointer == nullptr) {
            return false;
        }

        uint32 type = pointer->pointee;
        const Instruction* declaration = FindType(type);
        while (declaration != nullptr &&
               (declaration->opcode == OpTypeArray || declaration->opcode == OpTypeRuntimeArray)) {
            type = input_[declaration->offset + 2];
            declaration = FindType(type);
        }

        if (declaration == nullptr) {
            return false;
        }

        if (variable.storage == StorageUniform) {
            if (FindDecoration(type, DecorationBufferBlock) != nullptr) {
                kind = graphics::ShaderBindingKind::StorageBuffer;
                return true;
            }

            kind = graphics::ShaderBindingKind::ConstantBuffer;
            return FindDecoration(type, DecorationBlock) != nullptr;
        }

        switch (declaration->opcode) {
        case OpTypeImage:
            kind = declaration->count > ImageSampledWord &&
                           input_[declaration->offset + ImageSampledWord] == ImageSampledStorage
                       ? graphics::ShaderBindingKind::StorageImage
                       : graphics::ShaderBindingKind::SampledImage;
            return true;
        case OpTypeSampledImage:
            kind = graphics::ShaderBindingKind::SampledImage;
            return true;
        case OpTypeSampler:
            kind = graphics::ShaderBindingKind::Sampler;
            return true;
        default:
            return false;
        }
    }

    // set = 단계, binding = 종류 순서로 다시 매긴다(ShaderSystem 7.2절 4항).
    Result<void> RenumberResources(graphics::ShaderStage stage) noexcept {
        for (const Variable& variable : variables_) {
            if (variable.id == rootId_ ||
                (variable.storage != StorageUniformConstant && variable.storage != StorageUniform &&
                 variable.storage != StorageStorageBuffer)) {
                continue;
            }

            const Decoration* set = FindDecoration(variable.id, DecorationDescriptorSet);
            const Decoration* binding = FindDecoration(variable.id, DecorationBinding);
            if (set == nullptr || binding == nullptr) {
                continue;
            }

            Resource resource;
            resource.id = variable.id;
            resource.set = input_[set->literal];
            resource.binding = input_[binding->literal];
            if (!Classify(variable, resource.kind)) {
                Append(diagnostics_, StringView("unsupported resource type for SPIR-V id "));
                AppendNumber(diagnostics_, variable.id);
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            auto pushed = resources_.TryPushBack(resource);
            if (!pushed) {
                return pushed;
            }
        }

        resources_.Sort([](const Resource& left, const Resource& right) noexcept {
            if (left.kind != right.kind) {
                return left.kind < right.kind;
            }

            if (left.set != right.set) {
                return left.set < right.set;
            }

            return left.binding != right.binding ? left.binding < right.binding
                                                 : left.id < right.id;
        });
        // compute 파이프라인은 단계가 하나라 set 0이다(ShaderSystem 7.2절 4항: VS 0, PS 1, CS 0).
        const uint32 newSet = stage == graphics::ShaderStage::Pixel ? 1u : 0u;
        for (usize index = 0; index < resources_.Size(); ++index) {
            const Decoration* set = FindDecoration(resources_[index].id, DecorationDescriptorSet);
            const Decoration* binding = FindDecoration(resources_[index].id, DecorationBinding);
            words_[set->literal] = newSet;
            words_[binding->literal] = static_cast<uint32>(index);
        }

        return Result<void>::Success();
    }

    Result<void> Emit(Array<uint32>& output) noexcept {
        output.Clear();
        auto reserved = output.TryReserve(words_.Size() + insertions_.Size() * 4);
        if (!reserved) {
            return reserved;
        }

        words_[BoundWord] = bound_;
        auto header = output.TryAppend(Span<const uint32>(words_.Data(), HeaderWords));
        if (!header) {
            return header;
        }

        for (usize index = 0; index < instructions_.Size(); ++index) {
            const Instruction& instruction = instructions_[index];
            if (!removed_[index]) {
                auto copied = output.TryAppend(
                    Span<const uint32>(words_.Data() + instruction.offset, instruction.count));
                if (!copied) {
                    return copied;
                }
            }

            for (const Insertion& insertion : insertions_) {
                if (insertion.after == index) {
                    auto inserted = output.TryAppend(Span<const uint32>(insertion.words, 4));
                    if (!inserted) {
                        return inserted;
                    }
                }
            }
        }

        return Result<void>::Success();
    }

    Span<const uint32> input_;
    String& diagnostics_;
    Array<Instruction> instructions_;
    Array<Decoration> decorations_;
    Array<Pointer> pointers_;
    Array<Variable> variables_;
    Array<Access> accesses_;
    Array<Named> names_;
    Array<Named> semantics_;
    // 기록한 타입 선언 명령의 위치다.
    Array<usize> types_;
    Array<bool> removed_;
    Array<Insertion> insertions_;
    Array<Resource> resources_;
    // push constant로 바뀐 변수와 그것에서 나온 포인터다.
    Array<uint32> pushIds_;
    // 고쳐 쓰는 사본(입력과 같은 배치)이다.
    Array<uint32> words_;
    uint32 bound_ = 0;
    uint32 rootId_ = 0;
    uint32 executionModel_ = 0;
    bool hasEntryPoint_ = false;
};

} // namespace

Result<void> RewriteSpirv(Span<const uint32> input, graphics::ShaderStage stage,
                          StringView rootConstantsName, Array<uint32>& output,
                          String& diagnostics) noexcept {
    Rewriter rewriter(output.GetAllocator(), input, diagnostics);
    return rewriter.Run(stage, rootConstantsName, output);
}

} // namespace rocks::shader
