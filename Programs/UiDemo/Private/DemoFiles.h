#pragma once

#include <Core/Containers/Array.h>
#include <Core/Text/StringView.h>

namespace rocks::ui {

class VectorImageAtlas;

} // namespace rocks::ui

// 데모가 저장소에서 읽는 파일이다(시험 글꼴·ICU 데이터·갤러리 문자열). 경로는 엔진 폴더(Engine/)를
// 기준으로 하고, 엔진 폴더는 HostConfig FindEngineDirectory로 찾는다.
//
// 사용 예:
//   Array<uint8> bytes(allocator);
//   auto read = ReadEngineFile("Source/ThirdParty/IcuData/.../icudt79l.dat", bytes);
namespace rocks::ui {

// 엔진 폴더 기준 relative 파일 전체를 out에 읽는다. 엔진 폴더나 파일을 찾지 못하면 NotFound, 경로가
// 너무 길면 Overflow, 읽기 실패는 DependencyFailed다. 실패하면 out의 내용은 정하지 않는다.
Result<void> ReadEngineFile(StringView relative, Array<uint8>& out) noexcept;

// 엔진 아이콘 폴더(Content/Ui/Icons — Docs/Design/UiToolkit 17.3절)의 SVG를 모두 atlas에 등록하고
// 그 수를 돌려준다. 엔진 폴더를 찾지 못하면 NotFound, 등록 실패는 그 오류다.
Result<usize> RegisterEngineIcons(VectorImageAtlas& atlas) noexcept;

} // namespace rocks::ui
