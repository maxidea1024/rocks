#pragma once

#include <Core/Hash/Sha256.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/StringView.h>
#include <ShaderCompiler/ShaderSourceTree.h>

// 셰이더 원문 캐시다(Docs/Design/ShaderSystem 16.6절 SSD98). 전처리기는 include마다 파일을 읽고
// SHA-256·정규화를 한다. 이 캐시를 넘기면 같은 가상 경로는 캐시가 살아 있는 동안 한 번만 읽는다.
// 컴파일 관리자가 Compile 한 번마다 하나를 만들어 모든 job의 전처리에 넘긴다(감시 재컴파일의 다음
// Compile은 새 캐시라 바뀐 파일을 새로 읽는다).
//
// 사용 예:
//   ShaderSourceCache cache(tree, allocator);
//   PreprocessedShader shader(allocator);
//   auto done = PreprocessShader(tree, "/Engine/Private/Sample.rsf", defines, shader, {}, {},
//   &cache);
//
// 계약:
// - 여러 스레드가 동시에 Get을 불러도 된다. 잠금 안에서 찾고, 없으면 잠금 밖에서 읽은 뒤 먼저 넣은
//   쪽을 쓴다(같은 경로를 동시에 읽으면 한쪽 결과를 버린다).
// - 실패(없는 파일, 크기 초과, 인코딩 오류)도 캐시한다. 결과는 캐시 없이 읽은 것과 같은
// 상태·문구다.
// - 돌려준 항목과 그 원문은 캐시가 살아 있는 동안 바뀌지 않는다.
// - tree는 캐시보다 오래 살아야 하고, 캐시가 사는 동안 Mount하지 않는다.
// - 할당자는 여러 스레드의 동시 할당·해제를 받아야 한다.
namespace rocks::shader {

// 캐시한 원문 하나다. loadStatus가 실패면 원문이 없다(ShaderSourceTree::Load의 결과). 읽기는
// 됐지만 정규화가 실패하면 normalizeStatus가 실패다(위치는 그 파일의 줄).
struct ShaderSourceCacheEntry {
    // 정규화한 원문(앞 UTF-8 BOM 제거, 줄 끝 LF)이다.
    StringView text;
    // 원래 파일 byte의 SHA-256이다.
    Sha256Digest sha256{};
    ShaderSourceStatus loadStatus;
    ShaderSourceStatus normalizeStatus;
};

class ROCKS_SHADERCOMPILER_API ShaderSourceCache {
public:
    // fileBytes는 파일 하나의 크기 한도다. 이 캐시를 쓰는 전처리는 같은 한도여야 한다
    // (ShaderPreprocessLimits::fileBytes — 다르면 PreprocessShader가 InvalidArgument).
    ShaderSourceCache(const ShaderSourceTree& tree, Allocator& allocator,
                      usize fileBytes = MaxShaderSourceFileBytes) noexcept;
    ~ShaderSourceCache() noexcept;

    ShaderSourceCache(const ShaderSourceCache&) = delete;
    ShaderSourceCache& operator=(const ShaderSourceCache&) = delete;

    // virtualPath의 항목을 찾거나 읽어 넣는다. 할당이 실패하면 nullptr다.
    [[nodiscard]] const ShaderSourceCacheEntry* Get(StringView virtualPath) noexcept;

    [[nodiscard]] const ShaderSourceTree& Tree() const noexcept {
        return tree_;
    }

    [[nodiscard]] usize FileBytes() const noexcept {
        return fileBytes_;
    }

    // 캐시한 항목 수다(시험용).
    [[nodiscard]] usize EntryCount() const noexcept;

private:
    struct State;

    const ShaderSourceTree& tree_;
    Allocator& allocator_;
    usize fileBytes_ = 0;
    State* state_ = nullptr;
};

} // namespace rocks::shader
