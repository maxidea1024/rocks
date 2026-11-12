#pragma once

#include <Core/Error.h>
#include <Core/Move.h>
#include <Platform/Fatal.h>

namespace rocks {

// 성공값 T 하나 또는 ErrorCode를 보관하는 이동 전용 결과다. 별도 heap 할당은 없다.
// T의 이동·소멸은 noexcept여야 한다. 같은 Result의 동시 변경이나 T의 소멸 중
// 같은 Result를 다시 변경하는 재진입은 지원하지 않는다.
template <typename T>
class [[nodiscard]] Result {
    static_assert(IsNothrowMoveConstructibleV<T>);
    static_assert(IsNothrowDestructibleV<T>);

public:
    static Result Success(T value) noexcept {
        return Result(Move(value));
    }

    // 실패 객체에 성공 코드를 넣지 않도록 None은 InvalidArgument로 정규화한다.
    static Result Failure(ErrorCode error) noexcept {
        return Result(error == ErrorCode::None ? ErrorCode::InvalidArgument : error, FailureTag{});
    }

    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;

    // 이동 후에도 원본의 성공/실패 태그는 유지된다. 성공값은 T가 정의한 이동 후 상태가 된다.
    Result(Result&& other) noexcept : error_(other.error_) {
        if (other.IsOk()) {
            ConstructAt<T>(storage_, Move(other.Value()));
        }
    }

    Result& operator=(Result&& other) noexcept {
        if (this != &other) {
            ResetValue();
            error_ = other.error_;
            if (other.IsOk()) {
                ConstructAt<T>(storage_, Move(other.Value()));
            }
        }

        return *this;
    }

    ~Result() noexcept {
        ResetValue();
    }

    bool IsOk() const noexcept {
        return error_ == ErrorCode::None;
    }

    explicit operator bool() const noexcept {
        return IsOk();
    }

    // 성공을 먼저 확인해야 한다. 실패 상태의 접근은 Fatal이며 자동 기본값을 제공하지 않는다.
    // 반환 참조는 보관값의 수명에 종속되므로 결과의 대입·소멸을 넘어 보관하지 않는다.
    T& Value() & noexcept {
        CheckValue();
        return *reinterpret_cast<T*>(storage_);
    }

    const T& Value() const& noexcept {
        CheckValue();
        return *reinterpret_cast<const T*>(storage_);
    }

    ErrorCode Error() const noexcept {
        return error_;
    }

private:
    explicit Result(T&& value) noexcept : error_(ErrorCode::None) {
        ConstructAt<T>(storage_, Move(value));
    }

    // T 자체가 ErrorCode인 경우에도 값 생성과 실패 생성을 구분한다.
    struct FailureTag {};

    explicit Result(ErrorCode error, FailureTag) noexcept : error_(error) {
    }

    void CheckValue() const noexcept {
        if (!IsOk()) {
            platform::Fatal("Result::Value called on failure");
        }
    }

    void ResetValue() noexcept {
        if (IsOk()) {
            DestroyAt(reinterpret_cast<T*>(storage_));
        }
    }

    alignas(T) unsigned char storage_[sizeof(T)];
    ErrorCode error_;
};

// 반환값이 없는 작업의 성공/실패도 같은 명시적 검사 경로를 사용한다.
template <>
class [[nodiscard]] Result<void> {
public:
    static Result Success() noexcept {
        return Result(ErrorCode::None, FailureTag{});
    }

    // 실패 객체에 성공 코드를 넣지 않도록 None은 InvalidArgument로 정규화한다.
    static Result Failure(ErrorCode error) noexcept {
        return Result(error == ErrorCode::None ? ErrorCode::InvalidArgument : error, FailureTag{});
    }

    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;
    Result(Result&&) noexcept = default;
    Result& operator=(Result&&) noexcept = default;

    bool IsOk() const noexcept {
        return error_ == ErrorCode::None;
    }

    explicit operator bool() const noexcept {
        return IsOk();
    }

    ErrorCode Error() const noexcept {
        return error_;
    }

private:
    struct FailureTag {};

    explicit Result(ErrorCode error, FailureTag) noexcept : error_(error) {
    }

    ErrorCode error_;
};

} // namespace rocks
