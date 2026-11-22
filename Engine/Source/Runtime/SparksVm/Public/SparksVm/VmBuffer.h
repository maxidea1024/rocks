#pragma once

#include <Base/Types.h>
#include <Core/Text/StringView.h>

// VM의 값 버퍼다(Docs/Design/SparksVm.ko.md 2.2절, LC5b). 문자열·컨테이너·struct·Buf Result처럼
// Buf bank에 놓이는 값은 참조계수 COW 버퍼 하나다. 머리 바로 뒤에 내용(문자열은 UTF-8 byte,
// 컨테이너는 원소 slot 묶음)이 이어진다.
//
// 수명: 버퍼는 그것을 만든 VmExecution이 소유한다(ledger가 센다). host는 VmExecution::NewString으로
// 만들고 Release로 놓으며, 반환 값은 ReturnedBuffers로 빌려 읽는다. 다른 실행 문맥에 넘기지 않는다.

namespace rocks::sparks {

// 버퍼 종류다.
enum class VmBufferKind : uint8 {
    String,
    Array,
    Map,
    Set,
    Struct,
    Result,
    TagSet
};

// 단일 버퍼의 원소 수·byte 상한이다(SparksVm 2.2절).
inline constexpr uint64 MaxBufferElements = 16777216;
inline constexpr uint64 MaxBufferBytes = uint64(64) * 1024 * 1024;

// 버퍼 하나의 머리로 세는 byte다(ledger·예산 — 플랫폼마다 같게 실제 머리 크기 대신 이 값을 센다).
inline constexpr uint64 VmBufferHeaderBytes = 64;

// session 밖에서 VmExecution을 바로 부를 때의 기본 메모리 예산(byte)이다(session은
// VmSessionBudgets).
inline constexpr uint64 DefaultMemoryBudget = uint64(16) * 1024 * 1024;

struct VmBuffer {
    uint32 references = 0;
    VmBufferKind kind = VmBufferKind::String;
    // 컨테이너 원소 배치(값 bank·slot 수, key bank·slot 수 — 컨테이너 표지와 같은 뜻, LC5b2).
    uint8 valueBank = 0;
    uint8 valueSlots = 0;
    uint8 keyBank = 0;
    uint8 keySlots = 0;
    // 원소 수(문자열은 byte 수, struct는 slot 수, Result는 1 + 값 slot 수).
    uint32 count = 0;
    // 내용 자리의 원소 수(struct는 끝의 Buf slot 수 — 원소 버퍼로 놓는 자리).
    uint32 capacity = 0;
    uint32 indexCapacity = 0; // Map·Set 해시 색인의 칸 수(2의 거듭제곱, 원소 배열 뒤 — LC5b2)
    uint64 bytes = 0;         // 할당 크기(머리 포함 — ledger·예산)
    VmBuffer* next = nullptr; // 해제 중 대기 목록(실패하지 않는 해제 — 할당 없이 잇는다)

    char* Bytes() noexcept {
        return reinterpret_cast<char*>(this + 1);
    }

    const char* Bytes() const noexcept {
        return reinterpret_cast<const char*>(this + 1);
    }
};

static_assert(sizeof(VmBuffer) <= VmBufferHeaderBytes, "VmBuffer header grew past its budget size");

// 문자열 버퍼의 내용이다(null이면 빈 view). 버퍼가 살아 있는 동안 유효하다.
inline StringView VmStringOf(const VmBuffer* buffer) noexcept {
    return buffer == nullptr ? StringView() : StringView(buffer->Bytes(), buffer->count);
}

} // namespace rocks::sparks
