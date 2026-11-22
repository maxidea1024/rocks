#include "VmFormat.h"

#include <Platform/Memory.h>
#include <SparksVm/Opcode.h>

// TOSTR 표기다(Docs/Design/SparksVm.ko.md 2.2절).

namespace rocks::sparks::detail {

namespace {

// 수학 타입 하나의 성분 묶음 수다(Transform은 Location 3·Rotation 4·Scale 3).
constexpr uint32 VmTransformGroups[] = {3, 4, 3};

double VmFormatDouble(uint64 bits) noexcept {
    return platform::BitCast<double>(bits);
}

// 성분 count개를 `(a, b, …)`로 붙인다.
void VmAppendTuple(StringBuilderBase& out, const uint64* slots, uint32 count) noexcept {
    out.Append("(");
    for (uint32 index = 0; index < count; ++index) {
        out.Append(index == 0 ? "" : ", ");
        VmAppendFloat(out, VmFormatDouble(slots[index]), false);
    }

    out.Append(")");
}

} // namespace

void VmAppendFloat(StringBuilderBase& out, double value, bool single) noexcept {
    // Core 서식의 최단 왕복 표기다. 고정 소수점 표기에 소수점이 없으면(정수값) `.0`을 붙인다.
    const usize start = out.ByteCount();
    if (single) {
        out.AppendFormat("{}", static_cast<float>(value));
    } else {
        out.AppendFormat("{}", value);
    }

    const StringView written(out.View().Data() + start, out.ByteCount() - start);
    for (usize index = 0; index < written.ByteCount(); ++index) {
        const char c = written.Data()[index];
        if (c == '.' || c == 'e' || c == 'E' || c == 'n' || c == 'i') {
            return;
        }
    }

    out.Append(".0");
}

VmFaultKind VmFormatScalars(StringBuilderBase& out, uint32 display, const uint64* slots,
                            uint32 count) noexcept {
    switch (display) {
    case static_cast<uint32>(ScalarKind::Bool):
        out.Append(slots[0] != 0 ? "true" : "false");
        return VmFaultKind::None;
    case static_cast<uint32>(ScalarKind::I8):
    case static_cast<uint32>(ScalarKind::I16):
    case static_cast<uint32>(ScalarKind::I32):
    case static_cast<uint32>(ScalarKind::I64):
        out.AppendFormat("{}", static_cast<int64>(slots[0]));
        return VmFaultKind::None;
    case static_cast<uint32>(ScalarKind::U8):
    case static_cast<uint32>(ScalarKind::U16):
    case static_cast<uint32>(ScalarKind::U32):
    case static_cast<uint32>(ScalarKind::U64):
        out.AppendFormat("{}", slots[0]);
        return VmFaultKind::None;
    case static_cast<uint32>(ScalarKind::F32):
        VmAppendFloat(out, VmFormatDouble(slots[0]), true);
        return VmFaultKind::None;
    case static_cast<uint32>(ScalarKind::F64):
        VmAppendFloat(out, VmFormatDouble(slots[0]), false);
        return VmFaultKind::None;
    case static_cast<uint32>(DisplayKind::Vec2):
    case static_cast<uint32>(DisplayKind::Vec3):
    case static_cast<uint32>(DisplayKind::Vec4):
    case static_cast<uint32>(DisplayKind::Quat):
    case static_cast<uint32>(DisplayKind::Rotator):
    case static_cast<uint32>(DisplayKind::Color):
    case static_cast<uint32>(DisplayKind::Mat4):
        VmAppendTuple(out, slots, count);
        return VmFaultKind::None;
    case static_cast<uint32>(DisplayKind::Transform): {
        out.Append("(");
        uint32 offset = 0;
        for (uint32 group = 0; group < sizeof(VmTransformGroups) / sizeof(VmTransformGroups[0]);
             ++group) {
            out.Append(group == 0 ? "" : ", ");
            VmAppendTuple(out, slots + offset, VmTransformGroups[group]);
            offset += VmTransformGroups[group];
        }

        out.Append(")");
        return VmFaultKind::None;
    }
    default:
        // enum(TYPE 구역의 case 이름 — LC5b3), 이름·Tag·표지·AssetId(host 이름 표 — LC5d).
        return VmFaultKind::ContractViolation;
    }
}

} // namespace rocks::sparks::detail
