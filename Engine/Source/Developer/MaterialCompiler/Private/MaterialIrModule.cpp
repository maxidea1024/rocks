#include <Core/Move.h>
#include <MaterialCompiler/MaterialIrModule.h>

// 머티리얼 IR 모듈 만들기다(Docs/Design/MaterialSystem 6.5·6.6·6.8절).
namespace rocks::material {

namespace {

// IR hash 바이트의 형식 표지다(형식을 바꾸면 올린다).
constexpr uint32 IrHashFormatVersion = 1;

// 깊이 우선 순회 칸 하나다(재귀 없이 — 그래프가 깊어도 스택을 넘지 않는다).
struct Visit {
    IrValueId value;
    uint8 nextOperand;
};

void WriteU32(Sha256& hasher, uint32 value) noexcept {
    uint8 bytes[4];
    for (uint32 index = 0; index < 4; ++index) {
        bytes[index] = static_cast<uint8>(value >> (index * 8));
    }

    hasher.Update(Span<const uint8>(bytes, 4));
}

void WriteU64(Sha256& hasher, uint64 value) noexcept {
    WriteU32(hasher, static_cast<uint32>(value));
    WriteU32(hasher, static_cast<uint32>(value >> 32));
}

void WriteText(Sha256& hasher, StringView text) noexcept {
    WriteU32(hasher, static_cast<uint32>(text.ByteCount()));
    hasher.Update(Span<const uint8>(reinterpret_cast<const uint8*>(text.Data()), text.ByteCount()));
}

} // namespace

Result<void> BuildIrModule(const IrBuilder& builder, Span<const IrResult> results,
                           IrModule& out) noexcept {
    out.values_.Clear();
    out.results_.Clear();
    out.hash_ = Sha256Digest{};
    const Span<const IrValue> source = builder.Values();
    for (usize index = 0; index < results.Size(); ++index) {
        if (results[index].value >= source.Size() ||
            static_cast<uint32>(results[index].entry) >= IrEntryCount) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        for (usize other = 0; other < index; ++other) {
            if (results[other].entry == results[index].entry &&
                results[other].slot == results[index].slot) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }
    }

    // 결과를 진입점 순서로 안정 정렬한다(진입점마다 넣은 순서를 지킨다).
    Array<IrResult> ordered(out.results_.GetAllocator());
    for (uint32 entry = 0; entry < IrEntryCount; ++entry) {
        for (const IrResult& result : results) {
            if (static_cast<uint32>(result.entry) == entry) {
                if (auto pushed = ordered.TryPushBack(result); !pushed) {
                    return pushed;
                }
            }
        }
    }

    // 깊이 우선 후위 순으로 새 번호를 매긴다(피연산자가 먼저 — 6.6·6.8절).
    Array<IrValueId> remap(out.values_.GetAllocator());
    if (auto sized = remap.TryResize(source.Size()); !sized) {
        return sized;
    }

    for (IrValueId& slot : remap) {
        slot = NoIrValue;
    }

    Array<Visit> stack(out.values_.GetAllocator());
    for (IrResult& result : ordered) {
        if (remap[result.value] == NoIrValue) {
            if (auto pushed = stack.TryPushBack(Visit{result.value, 0}); !pushed) {
                return pushed;
            }
        }

        while (!stack.IsEmpty()) {
            Visit& top = stack[stack.Size() - 1];
            const IrValue& value = source[top.value];
            if (top.nextOperand < value.operandCount) {
                const IrValueId operand = value.operands[top.nextOperand++];
                // IR은 순환이 없으므로 아직 번호가 없는 피연산자는 스택 위 조상이 아니다.
                if (remap[operand] == NoIrValue) {
                    if (auto pushed = stack.TryPushBack(Visit{operand, 0}); !pushed) {
                        return pushed;
                    }
                }

                continue;
            }

            const IrValueId visited = top.value;
            (void)stack.TryPopBack();
            if (remap[visited] != NoIrValue) {
                continue;
            }

            IrValue copy = value;
            for (uint8 index = 0; index < copy.operandCount; ++index) {
                copy.operands[index] = remap[copy.operands[index]];
            }

            remap[visited] = static_cast<IrValueId>(out.values_.Size());
            if (auto pushed = out.values_.TryPushBack(copy); !pushed) {
                out.values_.Clear();
                return pushed;
            }
        }

        result.value = remap[result.value];
    }

    out.results_ = Move(ordered);

    Sha256 hasher;
    WriteU32(hasher, IrHashFormatVersion);
    WriteU32(hasher, static_cast<uint32>(out.values_.Size()));
    for (const IrValue& value : out.values_) {
        const uint8 header[] = {static_cast<uint8>(value.type.scalar), value.type.components,
                                value.type.rows, static_cast<uint8>(value.op), value.operandCount};
        hasher.Update(Span<const uint8>(header, sizeof(header)));
        for (uint8 index = 0; index < value.operandCount; ++index) {
            WriteU32(hasher, value.operands[index]);
        }

        for (uint64 immediate : value.immediate) {
            WriteU64(hasher, immediate);
        }
    }

    WriteU32(hasher, static_cast<uint32>(out.results_.Size()));
    for (const IrResult& result : out.results_) {
        WriteU32(hasher, static_cast<uint32>(result.entry));
        WriteText(hasher, result.slot);
        WriteU32(hasher, result.value);
    }

    out.hash_ = hasher.Finish();
    return Result<void>::Success();
}

} // namespace rocks::material
