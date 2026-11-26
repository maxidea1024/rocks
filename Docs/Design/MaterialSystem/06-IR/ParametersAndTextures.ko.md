# 머티리얼 시스템(MaterialSystem) 설계

## 6. IR — MS2c 구현 세부

### 6.10 MS2c 구현 세부(파라미터·텍스처·scene·선행 진단)

MS2c는 네 묶음으로 나눈다. 묶음마다 시험을 함께 넣고, 리뷰는 MS2c 끝에 한 번 받는다.

| 묶음 | 내용 |
|---|---|
| MS2c1 | 늦은 진단: TODO 노드(`PrerequisiteMissing`)와 모르는 노드(V12)를 static 해석·DCE 뒤 살아 있는 것에만 낸다(19.2.1절). 조건 값 가지에만 있으면 경고 |
| MS2c2 | 파라미터: scalar·vector·double vector·channel mask·static bool·static switch·static component mask 파라미터, 파라미터 컬렉션 값·변환, IR 파라미터 값(`Uniform` 빈도), static 값 입력, 상수 식 프로그램(9.4절)·상수 블록 layout, V8 |
| MS2c3 | 텍스처: IR 자원 타입, 텍스처 객체·파라미터, TextureSample과 파라미터 샘플 노드, TextureProperty, AntialiasedTextureMask, RecordTextureStreamingInfo, ParticleSubUv, V9(텍스처 예산)·V10 |
| MS2c4 | scene·깊이·버퍼: SceneTexture·SceneColor·UserSceneTexture·SceneDepth·DepthFade·SceneDepthWithoutWater·DecalBufferTexture·PathTracingBufferTexture(영역·블렌드 모드 검사 V6) |

**MS2c1 늦은 진단.**
- **자리 값.** 번역기는 TODO 노드(정의 상태 `PrerequisiteMissing`)와 카탈로그에 없는 노드를 만나면 바로 진단하지 않는다. 대신 받는 쪽이 요청한 출력 핀마다 IR 자리 값 `Pending`(번호 84)을 낸다. 즉시값 [0]은 종류(0 모르는 노드, 1 선행 없음), [1]은 선행 ID 번호(`MaterialPrerequisite`), [2]·[3]은 노드 NodeId다. NodeId를 키에 넣어 서로 다른 노드의 자리가 hash-consing으로 합쳐지지 않는다(진단 위치가 사라지지 않는다). 타입은 정의 출력 핀의 수 타입이고, `Numeric`·`Any`·모르는 노드는 Float4다.
- **진단.** 모듈을 만든 뒤(static 해석은 늦은 입력으로 이미 끝났고, DCE는 진입점에서 닿는 값만 남긴다) 결과에서 닿는 `Pending`마다 진단한다. 모르는 노드는 V12("Node type is not registered"), TODO 노드는 `PrerequisiteMissing`(108 — "Node is not translated until prerequisite <ID> exists")다.
  - 조건 값(Bool 문맥 값)을 조건으로 한 `Select`의 한 가지로만 닿으면 **경고**다(19.2.1절 "이 조합에서만 오류" — 그 가지를 고르는 (형식, pass) 조합의 거부는 쿠킹 MS3이 한다). 그 밖으로 닿으면 오류이고 번역이 실패한다.
  - 경고만 있으면 모듈이 남는다. 생성 원문은 자리 값을 그 타입의 0으로 낸다(조합 거르기가 그 가지를 쓰는 job을 만들지 않으므로 실제로 그려지지 않는다 — "검은 표면 금지" 규칙은 쿠킹 쪽 거부로 지킨다).
- UE 근거: 쓰지 않는 노드는 컴파일되지 않으므로 UE도 그 노드의 오류를 내지 않는다(classic은 활성 속성에서 닿는 식만 컴파일). Quality Switch 검증 컴파일(`MaterialExpressions.cpp:9088–9102`)의 모든 가지 진단은 에디터 경고 등급이다(14장 E4).
- `NotYetLowered`(이후 단계 노드)는 그대로 즉시 오류다(구현 중 표지 — MS2가 끝나면 남지 않는다).
