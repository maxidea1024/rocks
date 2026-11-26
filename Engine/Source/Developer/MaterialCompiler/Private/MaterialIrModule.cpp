#include <Core/Move.h>
#include <MaterialCompiler/MaterialIrModule.h>

// 머티리얼 IR 모듈 만들기다(Docs/Design/MaterialSystem 6.5·6.6·6.8절).
namespace rocks::material {

namespace {

// IR hash 바이트의 형식 표지다(형식을 바꾸면 올린다).
constexpr uint32 IrHashFormatVersion = 2;

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

namespace {

// 실패 결과를 돌려주기 전에 out을 비운다(반쯤 찬 모듈을 남기지 않는다).
Result<void> FailBuild(IrModule& out, Result<void> error) noexcept {
    out.Clear();
    return error;
}

} // namespace

Result<void> BuildIrModule(const IrBuilder& builder, Span<const IrResult> results,
                           IrModule& out) noexcept {
    out.Clear();
    const Span<const IrValue> source = builder.Values();
    for (usize index = 0; index < results.Size(); ++index) {
        if (results[index].value >= source.Size() ||
            static_cast<uint32>(results[index].entry) >= IrEntryCount) {
            return FailBuild(out, Result<void>::Failure(ErrorCode::InvalidArgument));
        }

        for (usize other = 0; other < index; ++other) {
            if (results[other].entry == results[index].entry &&
                results[other].slot == results[index].slot) {
                return FailBuild(out, Result<void>::Failure(ErrorCode::InvalidArgument));
            }
        }
    }

    // 결과를 진입점 순서로 안정 정렬한다(진입점마다 넣은 순서를 지킨다).
    Array<IrResult> ordered(out.results_.GetAllocator());
    for (uint32 entry = 0; entry < IrEntryCount; ++entry) {
        for (const IrResult& result : results) {
            if (static_cast<uint32>(result.entry) == entry) {
                if (auto pushed = ordered.TryPushBack(result); !pushed) {
                    return FailBuild(out, Move(pushed));
                }
            }
        }
    }

    // 깊이 우선 후위 순으로 새 번호를 매긴다(피연산자가 먼저 — 6.6·6.8절).
    Array<IrValueId> remap(out.values_.GetAllocator());
    if (auto sized = remap.TryResize(source.Size()); !sized) {
        return FailBuild(out, Move(sized));
    }

    for (IrValueId& slot : remap) {
        slot = NoIrValue;
    }

    Array<Visit> stack(out.values_.GetAllocator());
    for (IrResult& result : ordered) {
        if (remap[result.value] == NoIrValue) {
            if (auto pushed = stack.TryPushBack(Visit{result.value, 0}); !pushed) {
                return FailBuild(out, Move(pushed));
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
                        return FailBuild(out, Move(pushed));
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
                return FailBuild(out, Move(pushed));
            }
        }

        result.value = remap[result.value];
    }

    out.results_ = Move(ordered);

    // 진입점마다 닿는 문맥 값의 공급 기능과 필요 기능 등급이다(6.9절). 피연산자 번호는 늘 그 값보다
    // 작으므로 큰 번호부터 내려가며 표시를 퍼뜨리면 한 번에 끝난다. 픽셀 진입점에서 보간 값을
    // 만나면 그 아래(정점 값)는 Vertex 진입점의 사용으로 센다(6.9절 MS2b4a).
    const Span<const MaterialContextValue> table = MaterialContextValues();
    Array<bool> reached(out.values_.GetAllocator());
    Array<bool> interpolated(out.values_.GetAllocator());
    if (auto sized = reached.TryResize(out.values_.Size()); !sized) {
        return FailBuild(out, Move(sized));
    }

    if (auto sized = interpolated.TryResize(out.values_.Size()); !sized) {
        return FailBuild(out, Move(sized));
    }

    for (bool& item : interpolated) {
        item = false;
    }

    const auto account = [&](uint32 entry, Span<const bool> marks) noexcept -> Result<void> {
        for (usize index = 0; index < out.values_.Size(); ++index) {
            const IrValue& value = out.values_[index];
            if (!marks[index] || value.op != IrOp::Extern) {
                continue;
            }

            if (value.immediate[0] >= table.Size()) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            const MaterialContextValue& context = table[static_cast<usize>(value.immediate[0])];
            out.entryFeatures_[entry] |= ToMask(context.feature);
            if (static_cast<uint8>(context.minimumTier) > static_cast<uint8>(out.requiredTier_)) {
                out.requiredTier_ = context.minimumTier;
            }
        }

        return Result<void>::Success();
    };

    for (uint32 entry = 0; entry < IrEntryCount; ++entry) {
        for (bool& item : reached) {
            item = false;
        }

        bool any = false;
        for (const IrResult& result : out.results_) {
            if (static_cast<uint32>(result.entry) == entry) {
                reached[result.value] = true;
                any = true;
            }
        }

        if (!any) {
            continue;
        }

        for (usize index = out.values_.Size(); index > 0; --index) {
            if (!reached[index - 1]) {
                continue;
            }

            const IrValue& value = out.values_[index - 1];
            if (value.op == IrOp::Interpolate) {
                interpolated[value.operands[0]] = true;
                continue;
            }

            for (uint8 operand = 0; operand < value.operandCount; ++operand) {
                reached[value.operands[operand]] = true;
            }
        }

        if (auto counted = account(entry, reached.View()); !counted) {
            return FailBuild(out, Move(counted));
        }
    }

    // 보간 값 아래(정점 값)는 Vertex 진입점의 사용이다.
    for (usize index = out.values_.Size(); index > 0; --index) {
        if (!interpolated[index - 1]) {
            continue;
        }

        const IrValue& value = out.values_[index - 1];
        for (uint8 operand = 0; operand < value.operandCount; ++operand) {
            interpolated[value.operands[operand]] = true;
        }
    }

    if (auto counted = account(static_cast<uint32>(IrEntry::Vertex), interpolated.View());
        !counted) {
        return FailBuild(out, Move(counted));
    }

    // 보간 배정: 모듈 번호 순으로 성분을 이어 붙인다(6.4절 — UE GetVertexInterpolatorsOffsets).
    for (IrValue& value : out.values_) {
        if (value.op == IrOp::Interpolate) {
            value.immediate[0] = out.interpolantComponents_;
            out.interpolantComponents_ += value.type.components;
        }
    }

    // 파라미터 표: 닿는 Parameter의 표 항목을 ParameterId 순으로 모으고 즉시값 [3]을 다시 매긴다.
    // 컬렉션 표: 닿는 CollectionValue의 asset id 순(6.10절 MS2c2 — 결정적 layout).
    const Span<const IrParameter> sourceParameters = builder.Parameters();
    for (const IrValue& value : out.values_) {
        if (value.op == IrOp::Parameter) {
            const IrParameter& parameter = sourceParameters[static_cast<usize>(value.immediate[3])];
            bool known = false;
            for (const IrParameter& existing : out.parameters_) {
                known = known || existing.id == parameter.id;
            }

            if (!known) {
                if (auto added = out.parameters_.TryPushBack(parameter); !added) {
                    return FailBuild(out, Move(added));
                }
            }
        } else if (value.op == IrOp::CollectionValue) {
            const Guid asset{value.immediate[0], value.immediate[1]};
            bool known = false;
            for (const IrCollection& existing : out.collections_) {
                known = known || existing.asset == asset;
            }

            if (!known) {
                const IrCollection collection{asset, static_cast<uint32>(value.immediate[3])};
                if (auto added = out.collections_.TryPushBack(collection); !added) {
                    return FailBuild(out, Move(added));
                }
            }
        }
    }

    const auto less = [](Guid left, Guid right) noexcept {
        return left.high < right.high || (left.high == right.high && left.low < right.low);
    };
    for (usize index = 1; index < out.parameters_.Size(); ++index) {
        for (usize at = index; at > 0 && less(out.parameters_[at].id, out.parameters_[at - 1].id);
             --at) {
            const IrParameter swap = out.parameters_[at];
            out.parameters_[at] = out.parameters_[at - 1];
            out.parameters_[at - 1] = swap;
        }
    }

    for (usize index = 1; index < out.collections_.Size(); ++index) {
        for (usize at = index;
             at > 0 && less(out.collections_[at].asset, out.collections_[at - 1].asset); --at) {
            const IrCollection swap = out.collections_[at];
            out.collections_[at] = out.collections_[at - 1];
            out.collections_[at - 1] = swap;
        }
    }

    for (IrValue& value : out.values_) {
        if (value.op != IrOp::Parameter) {
            continue;
        }

        const Guid id{value.immediate[0], value.immediate[1]};
        for (usize index = 0; index < out.parameters_.Size(); ++index) {
            if (out.parameters_[index].id == id) {
                value.immediate[3] = index;
            }
        }
    }

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
