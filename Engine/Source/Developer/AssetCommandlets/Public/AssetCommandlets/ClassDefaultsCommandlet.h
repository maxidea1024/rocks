#pragma once

#include <Core/Commandlet/Commandlet.h>

// 클래스 기본값 덤프 명령이다(Docs/Design/RuntimeClassMetadata.ko.md 'A5c 지문 연결' 빌드 구성과
// 지문 — 대조 2). context는 AssetCommandContext*다(에셋 타입 모듈을 시작해 등록된 타입을 본다).
//   class-defaults-dump
//       에셋 타입 모듈(AssetCommandContext::typeModule — 비면 AssetTypeModule)과 그 의존 모듈이
//       등록한 구체 클래스마다 한 줄을 TypeId 순으로 표준 출력에 쓴다:
//       `<TypeId> present <cooked 지문 hex>`, `<TypeId> without <이유>`(레코드 없는 타입),
//       `<TypeId> failed <이유> <오류>`(레코드 실패). 검증 체인이 구성(Debug·Release·대상 종류)과
//       플랫폼마다 출력을 만들어 서로 비교한다(같은 대상 종류끼리 — 다르면 그 타입이 빌드 구성에
//       따라 다른 생성자 값을 가진다). 실패 줄이 있어도 종료 코드는 0이다(비교가 판정한다). 사용법
//       오류 2, 실행 실패 1.
namespace rocks::commandlet_host {

ROCKS_ASSETCOMMANDLETS_API Result<int32>
RunClassDefaultsDumpCommand(void* context, Span<const StringView> args,
                            CommandletOutput& output) noexcept;

} // namespace rocks::commandlet_host
