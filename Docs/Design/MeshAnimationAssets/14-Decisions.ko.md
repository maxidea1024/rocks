## 14. 결정 사항

| ID | 결정 | 버린 대안 |
|---|---|---|
| MA-D1 | 원본은 UE와 같은 다각형 + 삼각 분할 모델, 공유 가능한 코너 속성 방식 | 삼각형만 저장: n-gon·hard edge 정보를 잃어 재삼각화·모델링 도구가 불가능하다 |
| MA-D2 | 속성 하나 = Bulk entry 하나 | 메시 전체를 네이티브 blob 하나로(UE `FMeshDescriptionBulkData`): 리플렉션 바깥 직렬화 경로가 생기고, 빌드가 필요 없는 속성까지 풀어야 하며, 속성 단위 길이 검증·SHA 대조를 할 수 없다 |
| MA-D3 | `MeshSource`를 Runtime 모듈에 두고 AssetRuntime에 의존 | Developer에 두기: 에셋 클래스(Runtime)의 EditorOnly 필드 타입이 Runtime에서 보여야 한다. UE도 MeshDescription이 Runtime 모듈이다 |
| MA-D4 | 애니메이션 원본 키는 float, 프레임 균일, 프레임별 평가값 | double 저장: UE 원본 형식·런타임이 float다. Euler 채널 원본 저장: UE 내부 형식(FK Control Rig 채널)에 묶인다 |
| MA-D5 | MikkTSpace 참조 구현을 ThirdParty로 두고 초월 함수(`acos` 등)를 Core 결정적 수학으로 바꾸는 패치를 둔다. 패치 버전을 너겟 키에 넣는다 | 패치 없이 그대로: 호스트 libm에 따라 tangent 비트가 달라져 쿠킹 결정성이 깨진다. 자체 구현: 목적이 참조 구현과의 일치라 옮겨 적는 것과 차이가 없다 |
| MA-D6 | 소켓·충돌 형상은 값 구조체 배열 | UE처럼 UObject: 하위 객체 정체성이 필요한 기능(개별 참조·편집 transaction)이 아직 없다. 필요해지면 하위 객체로 옮긴다 |
| MA-D7 | 축소·lightmap UV·Nanite 등은 설정만 보존 | 지금 무시하고 빼기: 교환 형식으로 가져온 UE 설정이 사라져 나중에 다시 가져와야 한다 |
| MA-D8 | 퇴화 삼각형 판정·용접 임계값은 UE와 같은 식 | 면적 0 판정: 일직선 위의 서로 다른 세 점을 지워 UE와 위상이 달라진다 |
| MA-D9 | 위치 비유한값은 오류(UE는 0으로 고침) | UE처럼 고치기: 형상이 조용히 망가진다. 정상 콘텐츠에서는 나오지 않는다 |
| MA-D10 | index 형식은 GraphicsTypes `IndexFormat` 번호(MA2 전 이동, RenderBase R20 — 개정 4에서 바꿈) | Graphics `IndexFormat` 그대로 사용: 에셋 모듈이 RHI 모듈에 의존한다. Mesh 자체 열거(개정 2–3의 `MeshIndexWidth`): 같은 뜻의 열거가 둘이 된다 |
