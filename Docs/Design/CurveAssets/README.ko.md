# 커브 에셋(CR10)

개정 1(2026-11-27). [컴포넌트 도입](../ComponentCatalog/04-Prerequisites.ko.md) CR10의 설계다. 커브 값 타입 층([CurveData](../CurveData.ko.md) — `TangentCurve`·`PlainCurve`, CV1–CV10 완료) 위에 UE 5.8의 커브 에셋 층(`UCurveFloat`·`UCurveVector`·`UCurveLinearColor`·`UCurveTable`·`FRuntimeFloatCurve` 계열·`FCurveTableRowHandle`·`UCurveLinearColorAtlas`)을 들인다. CurveData 14절이 정한 경계를 따른다.

UE 경로 약어: `RE/` = `Engine/Source/Runtime/Engine/`, `UEd/` = `Engine/Source/Editor/UnrealEd/`(UE 5.8.4 — 로컬 클론 `C:\github\UnrealEngine`).

## 목차

1. [요약·범위·대응표·모듈](01-Overview.ko.md)
2. [커브 에셋 클래스(스칼라·벡터·색)](02-CurveAssetClasses.ko.md)
3. [인라인·에셋 커브 원천과 커브 표](03-SourcesAndTable.ko.md)
4. [색 커브 atlas](04-ColorCurveAtlas.ko.md)
5. [임포트·쿠킹·도구](05-ImportCook.ko.md)
6. [소비자 연결](06-Consumers.ko.md)
7. [결정·UE와 다른 점·단계·시험](07-DecisionsStagesTests.ko.md)

## 변경 이력

- 개정 1(2026-11-27): 설계 리뷰 반영. 원천은 참조 struct(객체 필드·StructRefArray만)이고 값 쪽은 평탄 값(CAD7·CAD9), 진동·입력 연결은 CA5 착수 때 방식 확정(6.1절 — 권장 slot 평탄화), 색 HSV는 UE float 식 따로(CAD10 — Core와 다름), 표 모드 불변식은 무시·쿠킹 비우기(CAD3), CSV는 UE 파서 규칙(CAD11), BOM·숫자 trim·비유한·JSON 항목 단위 건너뜀·진단 이름 `InvalidSource`·위치 정의, 쿠킹 Tangent 접선 재계산, atlas 너비 최소 2·역수 곱 표본·조정 중화·CA6 변경 목록, UE 순서·인용 정정.
- 개정 0(2026-11-27): 첫 초안. 사용자 결정(2026-11-27)으로 이 세션이 CR10을 맡는다. 에셋 세션(rocks-5f) 규약(리플렉션 클래스, 기본 쿠킹 처리기, A2f 임포터, CorruptData·InvalidArgument 구분)과 머티리얼 세션(rocks-2b) 요구(`CurveAtlasRowParameter`가 atlas 참조와 행 번호를 쓴다)를 반영했다.
