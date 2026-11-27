#include <Base/NumericLimits.h>
#include <MaterialCompiler/MaterialIrCustomTable.h>

// Custom HLSL 함수 표다(Docs/Design/MaterialSystem 6.13절 MS2f5).
namespace rocks::material {

namespace {

// 목록 번호다(칸 배열 안 순서).
constexpr uint32 CustomListArguments = 0;
constexpr uint32 CustomListOutputs = 1;
constexpr uint32 CustomListDefines = 2;
constexpr uint32 CustomListIncludes = 3;
constexpr uint32 CustomListCount = 4;

// UE가 본문에 return이 없으면 감싸는 앞뒤 원문이다(HT CustomExpression).
constexpr StringView CustomReturnPrefix = "return ";
constexpr StringView CustomReturnSuffix = ";";

bool IsCustomValueType(IrType type) noexcept {
    return !type.IsMatrix() && IsValidIrType(type) &&
           (type.scalar == IrScalar::Float || type.scalar == IrScalar::Double);
}

bool IsCustomArgumentType(IrType type) noexcept {
    return type.scalar == IrScalar::Texture ? type.texture != IrTextureKind::None
                                            : IsCustomValueType(type);
}

bool IsCustomFloatType(IrType type) noexcept {
    return IsCustomValueType(type) && type.scalar == IrScalar::Float;
}

} // namespace

StringView IrCustomTable::Text(IrText text) const noexcept {
    const usize end = static_cast<usize>(text.offset) + text.size;
    if (end > text_.Size()) {
        return StringView();
    }

    return StringView(text_.Data() + text.offset, text.size);
}

Span<const IrCustomItem> IrCustomTable::List(const IrCustomFunction& function, uint32 skip,
                                             uint32 count) const noexcept {
    const usize start = static_cast<usize>(function.firstItem) + skip;
    if (start + count > items_.Size()) {
        return Span<const IrCustomItem>();
    }

    return Span<const IrCustomItem>(items_.Data() + start, count);
}

Span<const IrCustomItem> IrCustomTable::Arguments(const IrCustomFunction& function) const noexcept {
    return List(function, 0, function.argumentCount);
}

Span<const IrCustomItem> IrCustomTable::Outputs(const IrCustomFunction& function) const noexcept {
    return List(function, function.argumentCount, function.outputCount);
}

Span<const IrCustomItem> IrCustomTable::Defines(const IrCustomFunction& function) const noexcept {
    return List(function, function.argumentCount + function.outputCount, function.defineCount);
}

Span<const IrCustomItem> IrCustomTable::Includes(const IrCustomFunction& function) const noexcept {
    return List(function, function.argumentCount + function.outputCount + function.defineCount,
                function.includeCount);
}

Result<IrText> IrCustomTable::Store(StringView text) noexcept {
    if (text_.Size() + text.ByteCount() > NumericLimits<uint32>::Max()) {
        return Result<IrText>::Failure(ErrorCode::InvalidArgument);
    }

    const IrText stored{static_cast<uint32>(text_.Size()), static_cast<uint32>(text.ByteCount())};
    if (auto appended = text_.TryAppend(Span<const char>(text.Data(), text.ByteCount()));
        !appended) {
        return Result<IrText>::Failure(appended.Error());
    }

    return Result<IrText>::Success(stored);
}

Result<void> IrCustomTable::Open(Guid node, IrCustomContext context, IrType returnType,
                                 bool pixelDiscard) noexcept {
    if (open_ || !IsCustomFloatType(returnType)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    pendingItems_ = items_.Size();
    pendingText_ = text_.Size();
    pendingList_ = CustomListArguments;
    pending_ = IrCustomFunction{};
    pending_.node = node;
    pending_.context = context;
    pending_.returnType = returnType;
    pending_.pixelDiscard = pixelDiscard;
    pending_.firstItem = static_cast<uint32>(items_.Size());
    open_ = true;
    return Result<void>::Success();
}

Result<void> IrCustomTable::Begin(Guid node, IrCustomContext context, IrType returnType,
                                  StringView code, bool pixelDiscard) noexcept {
    if (auto opened = Open(node, context, returnType, pixelDiscard); !opened) {
        return opened;
    }

    // 본문: return이 없으면 return <code>;로 감싸고 \r\n을 \n으로 바꾼다(UE 순서 그대로).
    const bool wrap = !code.Contains("return", StringComparison::CaseSensitive);
    const usize start = text_.Size();
    const StringView parts[] = {wrap ? CustomReturnPrefix : StringView(), code,
                                wrap ? CustomReturnSuffix : StringView()};
    for (StringView part : parts) {
        for (usize at = 0; at < part.ByteCount(); ++at) {
            const char character = part.Data()[at];
            if (character == '\r' && at + 1 < part.ByteCount() && part.Data()[at + 1] == '\n') {
                continue;
            }

            if (auto pushed = text_.TryPushBack(character); !pushed) {
                Abandon();
                return pushed;
            }
        }
    }

    if (text_.Size() > NumericLimits<uint32>::Max()) {
        Abandon();
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    pending_.code = IrText{static_cast<uint32>(start), static_cast<uint32>(text_.Size() - start)};
    return Result<void>::Success();
}

Result<void> IrCustomTable::AddItem(uint32 list, IrCustomItem item) noexcept {
    if (!open_ || list < pendingList_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto pushed = items_.TryPushBack(item); !pushed) {
        return pushed;
    }

    pendingList_ = list;
    uint32* counts[] = {&pending_.argumentCount, &pending_.outputCount, &pending_.defineCount,
                        &pending_.includeCount};
    ++*counts[list];
    return Result<void>::Success();
}

Result<void> IrCustomTable::AddArgument(StringView name, IrType type) noexcept {
    if (!open_ || !IsCustomArgumentType(type)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto stored = Store(name);
    if (!stored) {
        return Result<void>::Failure(stored.Error());
    }

    return AddItem(CustomListArguments, IrCustomItem{stored.Value(), IrText{}, type});
}

Result<void> IrCustomTable::AddOutput(StringView name, IrType type) noexcept {
    if (!open_ || !IsCustomFloatType(type)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto stored = Store(name);
    if (!stored) {
        return Result<void>::Failure(stored.Error());
    }

    return AddItem(CustomListOutputs, IrCustomItem{stored.Value(), IrText{}, type});
}

Result<void> IrCustomTable::AddDefine(StringView name, StringView value) noexcept {
    if (!open_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto storedName = Store(name);
    if (!storedName) {
        return Result<void>::Failure(storedName.Error());
    }

    auto storedValue = Store(value);
    if (!storedValue) {
        return Result<void>::Failure(storedValue.Error());
    }

    return AddItem(CustomListDefines,
                   IrCustomItem{storedName.Value(), storedValue.Value(), IrFloat(1)});
}

Result<void> IrCustomTable::AddInclude(StringView path) noexcept {
    if (!open_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto stored = Store(path);
    if (!stored) {
        return Result<void>::Failure(stored.Error());
    }

    return AddItem(CustomListIncludes, IrCustomItem{IrText{}, stored.Value(), IrFloat(1)});
}

Result<uint32> IrCustomTable::End() noexcept {
    if (!open_) {
        return Result<uint32>::Failure(ErrorCode::InvalidArgument);
    }

    // 같은 (노드, 문맥, 인자 형)이면 기존 항목이다. 노드가 같으면 코드·이름·출력·정의도 같다.
    const Span<const IrCustomItem> arguments = Arguments(pending_);
    for (usize index = 0; index < functions_.Size(); ++index) {
        const IrCustomFunction& existing = functions_[index];
        if (!(existing.node == pending_.node) || existing.context != pending_.context ||
            existing.argumentCount != pending_.argumentCount) {
            continue;
        }

        const Span<const IrCustomItem> others = Arguments(existing);
        bool same = true;
        for (usize at = 0; same && at < arguments.Size(); ++at) {
            same = others[at].type == arguments[at].type;
        }

        if (same) {
            Abandon();
            return Result<uint32>::Success(static_cast<uint32>(index));
        }
    }

    if (auto pushed = functions_.TryPushBack(pending_); !pushed) {
        Abandon();
        return Result<uint32>::Failure(pushed.Error());
    }

    open_ = false;
    return Result<uint32>::Success(static_cast<uint32>(functions_.Size() - 1));
}

void IrCustomTable::Abandon() noexcept {
    if (!open_) {
        return;
    }

    // 줄이기는 할당하지 않는다.
    (void)items_.TryResize(pendingItems_);
    (void)text_.TryResize(pendingText_);
    open_ = false;
}

Result<uint32> IrCustomTable::CopyFrom(const IrCustomTable& source, uint32 index) noexcept {
    if (open_ || index >= source.functions_.Size()) {
        return Result<uint32>::Failure(ErrorCode::InvalidArgument);
    }

    const IrCustomFunction& function = source.functions_[index];
    if (auto opened =
            Open(function.node, function.context, function.returnType, function.pixelDiscard);
        !opened) {
        return Result<uint32>::Failure(opened.Error());
    }

    // 본문은 감싸기를 마친 원문 그대로다.
    auto code = Store(source.Text(function.code));
    if (!code) {
        Abandon();
        return Result<uint32>::Failure(code.Error());
    }

    pending_.code = code.Value();
    const auto copy = [&](uint32 list, Span<const IrCustomItem> items) noexcept -> Result<void> {
        for (const IrCustomItem& item : items) {
            auto name = Store(source.Text(item.name));
            if (!name) {
                return Result<void>::Failure(name.Error());
            }

            auto value = Store(source.Text(item.value));
            if (!value) {
                return Result<void>::Failure(value.Error());
            }

            if (auto added = AddItem(list, IrCustomItem{name.Value(), value.Value(), item.type});
                !added) {
                return added;
            }
        }

        return Result<void>::Success();
    };

    const Span<const IrCustomItem> lists[CustomListCount] = {
        source.Arguments(function), source.Outputs(function), source.Defines(function),
        source.Includes(function)};
    for (uint32 list = 0; list < CustomListCount; ++list) {
        if (auto copied = copy(list, lists[list]); !copied) {
            Abandon();
            return Result<uint32>::Failure(copied.Error());
        }
    }

    if (auto pushed = functions_.TryPushBack(pending_); !pushed) {
        Abandon();
        return Result<uint32>::Failure(pushed.Error());
    }

    open_ = false;
    return Result<uint32>::Success(static_cast<uint32>(functions_.Size() - 1));
}

} // namespace rocks::material
