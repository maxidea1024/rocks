## 12. 시험

### 12.1 Rocks 쪽(UE 없이)

- 플러그인 안에 손으로 만든 작은 묶음 fixture를 둔다(`Engine/Plugins/Importers/UnrealExchange/Tests/Bundles/`): 텍스처 형식 9종, 큐브·배열·볼륨, 손실 원본(옵션 유무), 정적 메시(n-gon·hard edge·공유 코너·UV 8채널·슬롯 이름 불일치·Hard 순환), 생성 LOD(`baked` + buildScale), 스켈레톤 병합, morph·스킨 profile, 애니메이션(상수 트랙·가상 본·없는 본·additive 기준 클립), 머티리얼(번역 가능·불가·굽기 있음·같은 이름 파라미터), 인스턴스 사슬, redirector, 경로 정리 규칙(`_Shared`·공백·64 byte 초과·예약 이름).
- 변조·실패 주입: blob 잘림·SHA 불일치·크기 불일치, 가변 배치 파싱 실패, JSON 필수 키 누락·타입 오류, 배열 길이와 counts 불일치, 범위 밖 번호, Build 순환, 깊이 초과, 없는 의존, 경로 충돌, `formatVersion` 불일치. JSON·blob 퍼징(리더는 오류로 끝나거나 통과하고, 통과한 것은 임포트가 정상 진단으로 끝난다).
- 결정성: 같은 묶음을 두 번 임포트(다른 스레드 수) → 같은 패키지 bytes. AssetId 발급기를 시험용 결정적 발급기로 바꿔 비교한다. ObjectImports 정렬이 AssetId 순이라 ID만 빼고 비교하면 bytes가 다르기 때문이다. 시험용 발급기 주입 지점은 요구 UXR6이다.
- `--only` 결과와 전체 임포트 결과의 경로·내용 일치.
- 재임포트: 묶음 재추출(같은 objectPath, 또는 redirector로 이어진 새 경로) → 같은 AssetId 유지, 참조 보존.

### 12.2 UE 쪽

- 플러그인 자기 시험(UE Automation): SHA-256 NIST 벡터, 숫자 서식 왕복, 엔진 콘텐츠(`/Engine/BasicShapes/Cube` 등 정적 메시, 엔진 기본 머티리얼, 텍스처) 추출 결과의 형식 검증, 같은 상태 두 번 추출의 bytes 일치(굽기 제외), `prepared` 추출이 원본 에셋 설정을 바꾸지 않음.
- 대조 시험(수동, UX 단계마다): 같은 콘텐츠를 UE와 Rocks에서 그렸을 때의 차이는 화면 마일스톤 이후 렌더 세션과 정한다.

### 12.3 golden 묶음(개정 15)

RocksExport로 만든 실제 묶음 `Engine/Plugins/Importers/UnrealExchange/Tests/Bundles/Golden.rxbundle`(약 280 KiB, blob은 `.gitattributes`의 binary 규칙)을 저장소에 두고, 늘 도는 EngineTests 시험 `UnrealExchange_GoldenBundle`이 읽는다.

- **내용(에셋 11개).** 엔진 `/Engine/BasicShapes/Cube`, `/Engine/EngineMeshes/SkeletalCube`·`SkeletalCube_Skeleton`과 생성 스크립트가 만든 `/Game/RxGolden/` 콘텐츠: glTF 관절 둘 기둥(`SK_Column` — 섞인 가중치·morph `Bulge`, 스켈레톤, 3키 클립, 범위 밖 PhysicsAsset), 퇴화 다각형 메시 `SM_Degenerate`(OBJ 면 1-2-2-3 — UE 자기 변), 머티리얼 `M_Golden`(상수·스칼라 파라미터)과 인스턴스 `MI_Golden`, 옛 경로 soft 참조(클립의 `RetargetSourceAsset`이 `SK_SoftTarget_Old`를 가리키고 redirector가 `SK_SoftTarget`으로 잇는다 — redirect 1개). 닫힘 없이(`-NoClosure`) 내보내므로 묶음 밖 의존(엔진 머티리얼·압축 설정 등)은 의존에만 있다.
- **시험.** 외부 묶음 시험과 같은 판정(11.7절 18번 — 읽기·임포트 순서·메시 적재·검증·빌드·스켈레톤·스킨 빌드·클립·머티리얼 구조·모든 객체 참조의 의존 연결)을 돌리고 기대 수를 대조한다(정적 메시 기록 4·스킨 기록과 클립 빌드 5·머티리얼 기록 2). 경계 사례: 퇴화 메시 원본의 변 수(UE 6개 − 자기 변 1개 = 5)와 `DegenerateEdgesDropped`, 클립 의존이 새 경로·`SkeletalMesh`·soft·묶음 안 대상, PhysicsAsset Unsupported.
- **메시 적재 경로.** 두 시험 모두 메시 기록을 MA1 저장 규칙 그대로 적재한다: `MeshSourceData`의 요소 수는 `counts`, 위상 bulk 하나는 위상 blob 일곱을 9.1절 순서로 이어 붙인 것, 속성 레코드는 기록 순서이고 값 bulk는 교환 blob bytes 그대로(배치가 같다), 이것을 시험 에셋(`TestMeshAsset`)으로 실제 패키지에 쓰고 다른 Registry에 로드한 뒤 `MeshSource::Load`(배치 판정·검증)를 부른다. UX3b 임포트가 할 저장과 같은 규칙이다.
- **다시 만들기.** UE 5.8 프로젝트에 RocksExport를 설치하고 차례로(각각 새 에디터 프로세스):
  1. `python Extras/UnrealExport/RocksExport/Scripts/Golden/make_column_gltf.py <작업 폴더>/SK_Column.gltf`
  2. `UnrealEditor-Cmd <프로젝트>.uproject -run=pythonscript -script="<저장소>/Extras/UnrealExport/RocksExport/Scripts/Golden/make_golden_content.py content <작업 폴더>/SK_Column.gltf"`
  3. `UnrealEditor-Cmd <프로젝트>.uproject -run=pythonscript -script="<저장소>/Extras/UnrealExport/RocksExport/Scripts/Golden/make_golden_content.py redirect"`(클립이 로드되지 않은 새 프로세스에서 옛 대상을 새 대상으로 합쳐 redirector를 남긴다)
  4. `UnrealEditor-Cmd <프로젝트>.uproject -run=RocksExport -Assets=/Game/RxGolden/...,/Engine/BasicShapes/Cube,/Engine/EngineMeshes/SkeletalCube,/Engine/EngineMeshes/SkeletalCube_Skeleton -Out=<저장소>/Engine/Plugins/Importers/UnrealExchange/Tests/Bundles/Golden.rxbundle -NoClosure`
  다시 만들면 GUID(머티리얼 expression·임포트)가 바뀌어 bytes는 달라진다. 시험은 bytes가 아니라 판정과 기대 수를 본다. 플러그인 출력 형식을 바꾸면 golden도 다시 만든다.
- **손으로 만든 `Basic.rxbundle`과의 관계.** `Basic.rxbundle`은 묶음 형식(10.4절 — 정렬·redirect·경로 대응·blob 배치·변조·실패 주입)을 시험하는 fixture로 남긴다. 그 `settings`·`data`는 blob 참조와 배치를 담기 위한 최소 모양이며 현재 추출 스키마(4.5·5.4·6.3·7.1·8.6절)가 아니다. 추출 스키마의 본문은 golden이 맡는다(바꾸면 변조 시험이 기대는 번호·blob 사용이 모두 달라진다).
- **golden에 없는 경계 사례.** 정점 0개 메시·다채널 Name·영향 12개 초과는 UE 에셋으로 만들 수 없어(빌드가 막거나 임포터가 줄인다) UE 자기 시험이 writer에서 본다(11.7절 26번). 비NFC 이름은 플러그인이 그대로 쓰고 정규화가 임포트(UX3b) 몫이라(5.4절 3번) 임포트 시험이 맡는다. 키 수가 다른 본 트랙은 데이터 모델이 만들지 않아 진단 규칙만 둔다(7.1절 9번). 일반 파일이 아닌 항목·JSON 합 상한·플러그인 error 진단은 리더 변조 시험(`UnrealExchange_NotRegularFiles`·`UnrealExchange_BundleJsonBudget`·`UnrealExchange_ExporterErrorFailsAsset`)이 본다.
