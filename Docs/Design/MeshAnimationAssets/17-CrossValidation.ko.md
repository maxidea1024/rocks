## 17. 교차 검증 기록(개정 2)

리뷰어 두 명이 병렬로 봤다. 설계 반례 리뷰(초안과 기존 스펙·코드 대조)와 UE 5.8.4 사실 확인(주장 약 170개 대조, 문제 27건)이다. 이 문서에 해당하는 지적과 조치는 다음과 같다.

| 지적 | 심각도 | 조치 |
|---|---|---|
| 참조 struct(`MeshMaterialSlot`)를 `Array`에 담음, 빌드 설정의 객체 참조(`DistanceFieldReplacementMesh`)가 LOD 배열을 참조 struct 배열로 만듦 | 차단 | `StructRefArray`로 바꿈, 대체 메시는 StaticMesh EditorOnly 필드 + Build 의존(4.1·4.3·5.1절) |
| 코너를 "다각형 꼭짓점 하나"로 정의했으나 UE vertex instance는 공유됨, "세 코너는 서로 다른 정점" 불변식이 퇴화 삼각형 보존과 모순 | 차단 | 코너 공유 명시, 정점 중복 금지 삭제(3.1절) |
| UE `ComputeTriangleTangentsAndNormals`가 퇴화 삼각형을 지운다는 근거가 틀림(실제는 정점 버퍼 생성 때 위치 일치 판정) | 중요 | 빌드 순서·판정식을 UE와 같게 고침(5.2절 3·9단계, MA-D8) |
| MikkTSpace 참조 구현의 CRT `acos`가 쿠킹 결정성을 깸 | 중요 | 결정적 수학 패치·너겟 키(MA-D5, 9.3절), 재샘플링 slerp도 같은 규칙(7.4절) |
| 클립 쿠킹이 스켈레톤 본 번호에 의존하는데 Hard 참조뿐이라 병합 뒤 오래된 너겟이 적중함, 메시·클립 너겟 키 표 없음 | 중요 | Skeleton 참조를 Hard + Build로(7.1절), 너겟 키 표(9.3절) |
| 색 형식을 BGRA로 고정(GL 프로필은 UByte4Unorm), `IndexFormat`이 Graphics에 있음 | 중요 | 프로필별 색 형식, 자체 `MeshIndexWidth`, 요구 MR7(5.3절, MA-D10) |
| 속성 검증 없음(offset·boneIndex·희소 번호·Name 길이·대소문자 중복) | 중요 | 3.4.1·3.5절, 시험 추가 |
| 클립 `frameCount`·frame rate 상한 없음(OOM·넘침) | 중요 | `ClipMaxFrames`·`ClipMaxFrameRate`(7.2절) |
| 가변 길이 값(Name·BoneInfluences·SparseVec3)의 bulk 배치가 정해지지 않음, 스킨 가중치 "배열 둘"과 `data` 하나 불일치 | 중요 | 3.4.1절 값 타입별 배치 |
| UE 위치·색 비유한값 처리 설명이 틀림(UE는 위치 0, 색 흰색) | 사실 | 3.3절 고침, 위치는 UE와 다르게 오류로 둔 이유 명시(MA-D9) |
| UE morph 법선 속성 이름이 `Morph-`로 같음 | 사실 | 3.2절 표 고침, `MorphNormal-`로 바꾸는 이유 명시 |
| 중요도 필드 넷, 설정 여섯, skin weight profile 기본값이 플랫폼별, `bAutoComputeLODScreenSize` 접근자, `GetHiResSourceModel` 행, ReferenceSkeleton 경로·행, `bOnlyOneRootAllowed` 조건, UAnimationAsset 위치, StaticMeshBuilder 행, Color 기본값 행 | 사실 | 해당 줄 고침 |
| 축소 판정이 "UE와 같다"는 과장(UE는 축소 모듈 판정) | 사실 | 근사임을 명시(4.2절) |
| 빈 `MeshSourceData` = 원본 없음이 정상 빈 메시와 구분되지 않음 | 사소 | `hasSource` 플래그(3.4.2·5.1절) |
| buildScale 음수 winding·tangent 규칙, UV 범위 진단, stable bone ID·inverse-bind 누락(계획 13.1·15절) | 사소 | 5.2절 8단계, 3.3절, 6.1·6.3절 |
| 쿠킹 영향 상한 8로 9–12번째 영향을 버리는 결정과 8비트 잔차 규칙 누락 | 사소 | 6.3절 |
| UV 8채널 + 영향 8 등이 정점 속성 상한 16을 채움 | 사소 | UV 쌍 묶기(5.3절) |
| 이름 없는 수치, enum 기본값 없음 | 사소 | 이름 있는 상수, `Invalid = 0` |
| `BulkReader` 타입 없음, MA-D2 근거 "속성 하나만 다시 쓰기"가 사실이 아님 | 사소 | `Package` 기반 API, 근거 고침 |
| additive 기준 클립 참조 종류 근거 없음 | 사소 | 쿠킹이 미리 계산하지 않아 Hard임을 명시(7.1절) |
| 다른 문서 요구 누락(미러·LOD 그룹·모듈 그래프·진단 등록) | 사소 | MR7–MR10 |
| 타입 이름이 UE에서 접두만 뺀 것 | 판단 | 빌드 설정·축소 설정·섹션 정보·스킨 asset은 업계 공용 서술 이름이라 유지(머리말) |
