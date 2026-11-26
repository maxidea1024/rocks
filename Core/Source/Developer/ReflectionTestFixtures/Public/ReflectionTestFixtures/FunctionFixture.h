#pragma once

#include <Base/Compiler.h>
#include <Core/Containers/Array.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <Meta/Annotations.h>
#include <Object/Object.h>
#include <Object/ObjectRefs.h>

namespace rocks::reflection_fixture {

// 리플렉션 함수(ROCKS_FUNCTION — Docs/Design/FunctionReflection.ko.md FR1) fixture다.
// MetaCompiler가 만든 호출 frame·thunk·실행 schema를 TypeRegistry에 등록해 FunctionCall로
// 부른다(FunctionCallGeneratedTests): 인스턴스·const(Pure)·static 함수, Result<void>·Result<T>
// 반환과 함수 자신의 실패, StringView·enum·struct·Array·ObjectHandle·WeakObjectRef 매개변수, 도구
// 이름(Name), private 함수(GENERATED_BODY friend), EditorOnly 함수, 클래스 밖 정의.
ROCKS_ENUM()
enum class CallMode : uint8 {
    Plain,
    Doubled
};

ROCKS_STRUCT()
struct CallPoint {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 x = 0;
    ROCKS_PROPERTY() int32 y = 0;
};

ROCKS_CLASS()
class CallTarget : public Object {
    ROCKS_GENERATED_BODY()

public:
    explicit CallTarget(Allocator& allocator) noexcept : label(allocator) {
    }

    ROCKS_PROPERTY() int32 total = 0;
    ROCKS_PROPERTY() String label;

    // total에 amount를 더한다.
    ROCKS_FUNCTION() void Add(int32 amount) noexcept {
        total += amount;
    }

    ROCKS_FUNCTION(Pure) int32 Total() const noexcept {
        return total;
    }

    // 빈 글자면 InvalidArgument(함수 자신의 실패)다. 도구 이름은 Rename이다.
    ROCKS_FUNCTION(Name = "Rename") Result<void> SetLabel(StringView text) noexcept;

    // values 합에 point.x·point.y를 더하고 mode가 Doubled면 두 배로 한다. 합이 음수면 Overflow다.
    ROCKS_FUNCTION()
    Result<int32> Sum(const Array<int32>& values, CallMode mode,
                      const CallPoint& point) const noexcept;

    ROCKS_FUNCTION() static int32 Twice(int32 value) noexcept {
        return value * 2;
    }

    // other와 weak가 모두 자기 handle인가다.
    ROCKS_FUNCTION() bool Same(ObjectHandle other, WeakObjectRef<CallTarget> weak) const noexcept {
        return other == Handle() && weak.Handle() == Handle();
    }

#if ROCKS_WITH_EDITOR
    ROCKS_FUNCTION(EditorOnly) void Reset() noexcept {
        total = 0;
    }
#endif

private:
    ROCKS_FUNCTION() void Bump() noexcept {
        total += 100;
    }
};

inline Result<void> CallTarget::SetLabel(StringView text) noexcept {
    if (text.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return label.TryAssign(text);
}

inline Result<int32> CallTarget::Sum(const Array<int32>& values, CallMode mode,
                                     const CallPoint& point) const noexcept {
    int32 sum = point.x + point.y;
    for (int32 value : values) {
        sum += value;
    }

    if (sum < 0) {
        return Result<int32>::Failure(ErrorCode::Overflow);
    }

    return Result<int32>::Success(mode == CallMode::Doubled ? sum * 2 : sum);
}

} // namespace rocks::reflection_fixture
