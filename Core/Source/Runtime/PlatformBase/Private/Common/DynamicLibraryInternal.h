#pragma once

#include <Platform/DynamicLibrary.h>

// DynamicLibrary의 플랫폼 hook이다. 인자 검사·lease 계수·수명은 Common/DynamicLibrary.cpp가,
// OS 로더 호출은 Private/<플랫폼>/DynamicLibrary.cpp가 맡는다.
namespace rocks::platform::detail {

// path는 공통 검사(null·빈 값·NUL·잘못된 UTF-8)를 통과한 UTF-8이다(NUL 종료 아님). 절대 경로
// 판정과 OS 열기·실패 분류를 한다. 성공하면 handle에 OS handle을 둔다.
FileStatus DynamicLibraryOpenPath(const char* path, usize pathBytes, uptr& handle) noexcept;

// name은 공통 검사(길이, 구분자·':'·"."·".." 없음, UTF-8)를 통과한 파일 이름이다.
FileStatus DynamicLibraryOpenSystem(const char* name, usize nameBytes, uptr& handle) noexcept;

// handle에서 symbol(NUL 종료, 비어 있지 않음)의 주소를 찾는다. 없으면 null이다.
void* DynamicLibraryFindSymbol(uptr handle, const char* symbol) noexcept;

// handle을 내린다. 실패해도 handle을 다시 쓰지 않는다.
FileStatus DynamicLibraryClose(uptr handle) noexcept;

} // namespace rocks::platform::detail
