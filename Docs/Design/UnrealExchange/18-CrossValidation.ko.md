## 18. 교차 검증 기록(개정 2)

리뷰어 두 명이 병렬로 봤다. 설계 반례 리뷰(초안과 기존 스펙·코드 대조)와 UE 5.8.4 사실 확인(주장 약 170개 대조, 문제 27건, 미확인 5건 결론)이다. 이 문서에 해당하는 지적과 조치는 다음과 같다.

| 지적 | 심각도 | 조치 |
|---|---|---|
| 경로 대응이 AssetSystem 4.2절(첫 문자 L*·N*, 64·256 byte)을 어김(`_Engine`, `_` 치환), 플러그인 마운트 대응 없음 | 차단 | 마운트를 첫 구성 요소로, 정리 규칙 1–5, 길이·충돌 오류(9.1절) |
| UE 준비 원본이 퇴화 삼각형을 지운다는 서술이 틀림 | 중요 | 지우지 않음을 명시, 위상이 원본과 같음(5.3절) |
| 생성 LOD를 렌더 데이터로 바꾸며 원본·축소 설정을 버림, 렌더 데이터에 이미 적용된 BuildScale3D를 다시 곱함 | 중요 | `source`·`reduction` 항상 기록, `baked`는 보조, 사용 때 buildScale 1(5.1절) |
| 번역 실패 결과 3번이 쿠킹 전체를 막음, MaterialSystem 11.3절 인용이 틀림 | 중요 | 기본 머티리얼 인스턴스로 대체(8.5절, UX-D9) |
| "ImportRecord 옆 보조 파일"이 AssetSystem에 없는 저장소, 구운 텍스처의 다중 출력 규칙 누락 | 중요 | 보조 파일 삭제, ImportRecord `outputs`·`ownerAssetId`(8.5절) |
| ImportRecord 원본 표기가 5.4절 스키마와 다름, AssetIndex가 objectPath로 찾을 수 없음 | 중요 | `sources` role 표기, `options`, 태그(9.2절, UXR2) |
| 같은 단계 안 Hard 참조·SCC·순환을 objectPath 순으로 처리 못 함 | 중요 | 전체 그래프 SCC 위상 정렬, 깊이 상한(9.3절) |
| `_2` 규칙이 처리 집합에 따라 달라짐 | 중요 | 해시 접미(9.1절, UX-D10) |
| UX-D4 근거가 재연결 방식과 맞지 않음, redirector 미처리 | 중요 | redirector 풀기·`redirects`·`UnrealExchange.FormerPaths` 태그, 근거 고침 |
| blob 원소 종류로 가변 길이 배치를 표현할 수 없음 | 중요 | `layout`에 `names`·`boneInfluences`·`sparseVec3`(3.3절) |
| UE 파라미터 이름 병합과 Rocks V8 충돌 | 중요 | 병합·끌어올리기 규칙(8.5절), UXR4 |
| `UStructToJsonObject`의 키 대소문자 변환·숫자 서식 | 중요 | 자체 덤프 writer(11.5절) |
| 굽기 결과가 GPU마다 달라 결정성 보장의 반례 | 중요 | 보장에서 제외, `environment` 기록(3.2·8.4절) |
| `SetupRenderMeshDescription`이 설정의 lightmap 번호를 고침, Nanite 메시 인자 | 중요 | 설정 사본, `bForNanite` 선택, `preparedBuild`(5.3절) |
| 3.2절 "최단 왕복"과 11.4절 모순, NaN 부호 손실 | 사소 | 3.2절 고침 |
| 절 번호 오인용(6.2→5.3, 13.3→9.3), MAT 단계 이름 | 사소 | 고침(MaterialSystem MS3·MS6) |
| notify 속성 보존 서술이 메시 문서와 불일치 | 사소 | 받지 않고 경고로 통일(7절) |
| 위상 blob 7개와 bulk 하나의 대응 | 사소 | 임포트가 이어 붙임(5.2절) |
| 결정성 시험의 AssetId 비교 규칙이 없음 | 사소 | 결정적 발급기 주입(12.1절, UXR6) |
| `--allow-lossy-origin`이 재임포트에 남지 않음 | 사소 | `options`에 기록(4.3절) — 2026-11-23 옵션 폐지로 해당 없음 |
| "다른 문서에 반영할 요구" 절 없음 | 사소 | 13절 |
| 굽기·Substrate를 v1에 모두 넣는 범위 | 사소 | UX5(Substrate)·UX6(굽기)로 분리 |
| Rocks 임포터 위치(핵심 계층) | 사소 | 엔진 플러그인(10.1절, UX-D11) |
| OpenSSL 링크 대안 | 사소 | 검토 후 자체 구현 유지(UX-D12) |
| UE 사실: IsLongLatCubemap·GetGammaSpace 행, BGRE8 해제식, UEDELTA 무손실 근거, RetainJpegFormat의 Interchange 경로, `SetupRenderMeshDescription` 멤버 함수, override 배열 9종, Substrate 변환 노드 범위, 굽기 HDR·크기 기본값, commandlet 렌더 플래그, JsonUtilities 의존, Editor 모듈 로드 근거, SHA-256 공개 API 범위 | 사실 | 해당 줄 고침 |
| 미확인 결론: 큐브 면 순서(D3D 순서), FJsonValueNumber 서식(`%.17g`·비유한값 깨짐), 스킨 빌더 사본 함수 없음, 생성 스킨 LOD 복원 가능, UMaterialEditorOnlyData Outer = 머티리얼 | 사실 | 4.1·3.2·6.2·8.1절에 반영, 미확인 표에서 지움 |
