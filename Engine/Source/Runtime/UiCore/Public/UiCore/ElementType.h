#pragma once

#include <Base/Types.h>

namespace rocks::ui {

// Element 형식 정보다(Docs/Design/UiToolkit 20.10절 CB2 — UE SLATE_DECLARE_WIDGET의 형식 정보).
// 클래스 선언의 ROCKS_UI_ELEMENT가 만들고, IsA·ElementCast·StaticElementCast가 부모 사슬을 따라
// 판정한다. 같은 형식인지는 주소 또는 이름으로 본다(Modular 링크에서 inline 정적 변수가 모듈마다
// 따로 생길 수 있다).
struct ElementType {
    const char* name;
    const ElementType* parent;
};

} // namespace rocks::ui

// Element 파생 클래스 본문 맨 앞에 쓴다(UE SLATE_DECLARE_WIDGET). Super·ThisClass 별칭과 형식
// 정보를 만든다. 끝에서 접근 지정이 private로 돌아간다(class 본문의 기본 접근과 같다).
//
// 사용 예:
//   class ROCKS_UIFRAMEWORK_API Button : public Frame {
//       ROCKS_UI_ELEMENT(Button, Frame)
//   public:
//       ...
//   };
#define ROCKS_UI_ELEMENT(ClassName, BaseName)                                                      \
public:                                                                                            \
    using Super = BaseName;                                                                        \
    using ThisClass = ClassName;                                                                   \
    static const ::rocks::ui::ElementType& StaticType() noexcept {                                 \
        static const ::rocks::ui::ElementType type{#ClassName, &BaseName::StaticType()};           \
        return type;                                                                               \
    }                                                                                              \
    const ::rocks::ui::ElementType& GetType() const noexcept override {                            \
        return StaticType();                                                                       \
    }                                                                                              \
                                                                                                   \
private:
