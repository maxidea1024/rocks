#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <SparksVm/Bytecode.h>

// `.sparkc` 파일 쓰기·읽기와 verifier다(Docs/Design/SparksBytecode.ko.md 5절, LC4c). 쓰기는
// 결정적이다 (같은 모듈 → 같은 byte). 읽기는 형식(magic·버전·구역 순서·길이)을 보고 모듈을 채운 뒤
// verifier를 돌린다. 거부는 모듈 단위이며 사유와 위치를 돌려준다.
//
// 사용 예:
//   Array<uint8> bytes(allocator);
//   if (auto written = WriteBytecodeFile(module, bytes); !written) { /* OutOfMemory 등 */ }
//   BytecodeModule loaded(allocator);
//   BytecodeRejection rejection;
//   auto read = ReadBytecodeFile(bytes.View(), loaded, rejection);
//   if (!read && read.Error() == ErrorCode::InvalidArgument) {
//       Log(BytecodeLoadErrorName(rejection.error), rejection.function, rejection.address);
//   }

namespace rocks::sparks {

// 거부 사유다(형식·verifier).
enum class BytecodeLoadError : uint8 {
    None,
    Truncated,          // 파일이 형식보다 짧다
    BadMagic,           // `SPKC`가 아니다
    UnsupportedVersion, // 형식 버전이 이 VM과 다르다
    BadSection,         // 구역 tag·순서·길이가 어긋난다, 끝 뒤에 byte가 있다
    BadTable,           // 표 번호(이름·문자열·함수)가 범위 밖이다
    BadFunction,        // 함수 구간·bank 크기·매개변수 수가 어긋난다
    BadOpcode,          // 모르는 opcode, 홀로 온 EXTRA, EXTRA 빠짐
    BadRegister,        // 레지스터 피연산자가 frame 밖이다
    BadConstant,        // 상수·문자열 번호가 범위 밖이다
    BadReference,       // 참조 번호가 범위 밖이거나 종류가 명령과 다르다
    BadBranch,          // 분기·TRY·switch·await 주소가 함수 안의 명령 시작이 아니다
    BadOperand,         // 변환 종류·컨테이너 표지·표시 종류가 범위 밖이다
    BadEnd              // 함수의 마지막 명령이 끝 명령이 아니다
};

const char* BytecodeLoadErrorName(BytecodeLoadError error) noexcept;

// 거부의 자리다(함수·명령 주소 — 해당이 없으면 NoBytecodeIndex).
struct BytecodeRejection {
    BytecodeLoadError error = BytecodeLoadError::None;
    uint32 function = NoBytecodeIndex;
    uint32 address = NoBytecodeIndex;
};

// module을 `.sparkc` byte로 out 끝에 덧붙인다. 실패: 할당 실패는 OutOfMemory, u32 길이를 넘는 표는
// CapacityExceeded.
Result<void> WriteBytecodeFile(const BytecodeModule& module, Array<uint8>& out) noexcept;

// bytes를 읽어 out(빈 모듈)을 채우고 verifier를 돌린다. 실패: 형식·verifier 거부는 InvalidArgument
// (사유는 rejection), 할당 실패는 OutOfMemory. 실패하면 out의 내용은 정해지지 않는다.
Result<void> ReadBytecodeFile(Span<const uint8> bytes, BytecodeModule& out,
                              BytecodeRejection& rejection) noexcept;

// 모듈의 구조를 검사한다(5절 verifier). 통과면 error가 None이다.
BytecodeRejection VerifyBytecode(const BytecodeModule& module) noexcept;

} // namespace rocks::sparks
