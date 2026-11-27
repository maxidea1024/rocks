# 커브 에셋 — 3. 인라인·에셋 커브 원천과 커브 표

[목차](README.ko.md)

## 3. 인라인·에셋 커브 원천과 커브 표

### 3.1 커브 원천 셋(UE `FRuntimeFloatCurve`·`FRuntimeVectorCurve`·`FRuntimeCurveLinearColor`)

인라인 커브와 커브 에셋 참조를 함께 가진다. 참조가 있으면 에셋 커브를 쓰고, 인라인 커브는 지우지 않고 남긴다(UE와 같다).

| struct | 인라인 | 참조 | UE |
|---|---|---|---|
| `FloatCurveSource` | `TangentCurve curve` | `ObjectRef<FloatCurveAsset> asset` | `EditorCurveData`·`ExternalCurve`(`CurveFloat.h:11-27`) |
| `VectorCurveSource` | `VectorCurveValue curves` | `ObjectRef<VectorCurveAsset> asset` | `VectorCurves[3]`·`ExternalCurve`(`CurveVector.h:11-27`) |
| `ColorCurveSource` | `TangentCurve r, g, b, a` | `ObjectRef<ColorCurveAsset> asset` | `ColorCurves[4]`·`ExternalCurve`(`CurveLinearColor.h:13-25`) |

- **참조 struct다(CAD7).** `ObjectRef`는 복사·이동이 막혀 있고, 소유 객체 밖의 버퍼(Array 원소)에 둘 수 없다(`Object/ObjectRefs.h:175-206`). 그래서 원천 struct를 둘 수 있는 자리는 두 곳뿐이다.
  - 리플렉션 클래스(에셋 객체)의 직접 필드
  - 소유 객체가 관리하는 `StructRefArray` 원소
- 원천 struct를 둘 수 없는 자리:
  - `Array` 원소
  - Map 값(RMC2305)
  - Optional 안(RMC2306)
  - `StructRefArray` 원소 안의 또 다른 `StructRefArray`(RMC2304 — ReflectionExtensions 9장)
  - 자유롭게 복사하는 값 struct
  - 이런 자리는 2.5절 값 struct(평탄 커브)를 쓴다.
- 참조는 Hard다(`ObjectRef` — AssetSystem 6.2절: 함께 로드·쿠킹된다). UE `TObjectPtr`(하드)와 같다. 버린 대안: Soft 참조. 에셋 안 참조가 함께 로드되지 않으면 평가 때 대상이 없다.
- World 객체는 에셋을 `ObjectRef`로 걸 수 없다(AssetSystem 6.3절). 그래서 World 컴포넌트(타임라인 — CM5)는 `SoftObjectRef<FloatCurveAsset>`와 `AssetLease`를 따로 쥐고 3.1.2절 사본으로 값을 꺼낸다. 세부는 그 컴포넌트를 설계할 때 정한다.
- 인라인 커브의 이름은 `curve`다. UE의 `EditorCurveData`는 이름과 달리 편집기 전용이 아니어서 쿠킹 뒤에도 남는다. 참조가 있어도 인라인은 저장·쿠킹된다(UE와 같다). 참조를 지우면 인라인이 다시 쓰이는 편집 동작 때문에 버리지 않는다.

#### 3.1.1 평가

Rocks 객체 참조는 포인터가 아니라 handle이다. 그래서 참조를 풀려면 그 에셋의 Registry와 접근 문맥이 필요하다. `Resolve`는 non-const이므로 Registry를 non-const로 받는다.

- `Result<float> FloatCurveSource::TryEvaluate(ObjectRegistry&, const ObjectAccessContext&, float time) const`
  - 참조가 null이면 `curve.Evaluate(time)`, 아니면 그 에셋의 `Evaluate(time)`이다.
  - 참조가 풀리지 않으면 Resolve 오류를 그대로 돌려준다. 원천이 강한 간선이라 대상은 회수되지 않으므로 실제로 나는 것은 파괴 요청된 대상의 `Destroyed`와 다른 스레드의 `WrongThread`다(다른 타입은 `SetReference`가 막는다).
  - UE는 외부 커브가 null일 때만 `GetRichCurveConst()`가 인라인으로 대신한다. 구조체에 `Eval`이 없어 소비자가 `Eval`을 직접 부른다.
  - Rocks는 풀리지 않는 참조를 인라인으로 조용히 대신하지 않고 오류로 돌려준다(CAD8, ◇ 7.2절 2).
- 벡터·색 원천도 같은 규칙이다.
  - `VectorCurveSource::TryEvaluate`는 `Result<Vec3d>`, `ColorCurveSource::TryEvaluate`는 `Result<LinearColor>`를 돌려준다.
  - 색은 참조가 있으면 **조정을 적용한** 값(`ColorCurveAsset::Evaluate`)이다.
  - 인라인이면 조정 필드가 없어 원값이고, 알파 키가 없으면 A = 1이다.
  - UE `FRuntimeCurveLinearColor::GetLinearColorValue`(`CurveLinearColor.cpp:15-38`)와 같다.
- 참조가 null인 원천은 Registry 없이 평가할 수 있다.
  - `bool HasAsset() const`
  - `float EvaluateInline(float time) const`(벡터·색도 같은 이름)

#### 3.1.2 사본(평탄 커브 만들기 — CAD9)

다른 Registry·스레드로 넘기거나 값 struct에 담을 때 쓴다.

- `Result<void> FloatCurveSource::TryFlattenInto(ObjectRegistry&, const ObjectAccessContext&, TangentCurve& out) const`
  - 쓰일 커브를 out에 복사한다. 참조가 있으면 에셋 커브, 없으면 인라인 커브다.
  - 실패하면 out은 그대로다.
- 벡터는 `VectorCurveValue`로 같은 일을 한다.
- 색은 `ColorCurveValue`로 같은 일을 한다.
  - 참조가 있으면 커브 넷과 조정 일곱을 복사하고 `adjusted` = 참이다.
  - 인라인이면 커브 넷을 복사하고 `adjusted` = 거짓이다.
- 오류: 3.1.1절의 Resolve 오류와 할당 실패(OutOfMemory)다.

### 3.2 `CurveTableAsset`(UE `UCurveTable`)

이름 붙은 스칼라 커브의 표다. 게임 수치(레벨별 값 등)를 한 에셋에 모은다.

#### 3.2.1 저장 형태(CAD3 — CurveData 14절 D2의 결정)

- 필드:
  - `ROCKS_PROPERTY() CurveTableMode mode = CurveTableMode::Empty;`
  - `ROCKS_PROPERTY() OrderedMap<Name, PlainCurve> plainRows;`
  - `ROCKS_PROPERTY() OrderedMap<Name, TangentCurve> tangentRows;`
- **모드가 쓸 맵을 정한다.** Plain이면 `plainRows`, Tangent면 `tangentRows`, Empty면 둘 다 쓰지 않는다.
  - 모드가 쓰지 않는 맵의 행은 조회·평가에서 없는 것으로 친다. 그래서 로드가 실패하지 않는다.
  - 경로 쓰기(`asset-set`)나 Inspector가 반대쪽 맵에 값을 써도 에셋은 열린다.
  - 편집 함수와 임포터는 반대쪽 맵을 늘 비워 둔다.
  - 쿠킹 처리기는 쓰지 않는 맵을 비우고 비어 있지 않았으면 경고(`CurveTableUnusedRows`)를 남긴다(5.3절).
  - 버린 대안: 로드 뒤 불변식 검사를 CorruptData로 처리하는 것.
    - 클래스 PostLoad 판정 기반이 아직 없다(A5k가 미뤘다 — AssetSystem 12.15).
    - 사람이 고친 텍스트를 손상으로 볼 수 없다(19장 — 사람이 쓴 텍스트 위반은 InvalidArgument).
- `OrderedMap`의 순서는 삽입 순서다. 새 키는 끝에 붙고, 지운 뒤 다시 넣으면 끝에 붙는다.
  - UE `TMap`(sparse set)은 지운 자리를 다시 쓴다(`SparseArray.h:120-126`). 그래서 지운 뒤의 순회 순서가 다르다(◇ 7.2절 12). Rocks 순서는 연산 순서만으로 정해진다.
- UE `RowMap`은 UPROPERTY가 아니라 `Serialize`가 손으로 쓴다(`CurveTable.cpp:201-235`). Rocks는 리플렉션 필드라 텍스트·차이 저장·Inspector가 그대로 동작한다.
- 버린 대안:
  - 행마다 `InstancedStruct<ScalarCurveBase>`(UE `FRealCurve*`처럼 다형). UE 표는 모드가 하나라 행 타입이 섞이지 않고, 다형 저장은 행마다 머리말·힙 블록 비용이 든다(CurveData 14절).
  - 맵 하나에 모드별 값. 리플렉션 맵의 값 타입은 정적이다.
- 행 커브는 맵 값(인라인 struct)이다. 그래서 UE처럼 따로 `new`하지 않는다.

#### 3.2.2 조회

UE `FindCurve`·`FindRichCurve`·`FindSimpleCurve`·`FindCurveUnchecked`에 대응한다.

- `const ScalarCurveBase* FindRow(Name row) const`: 모드가 쓰는 맵에서 찾는다. 없으면 null이고 로그를 남기지 않는다(UE `FindCurveUnchecked`).
- `const PlainCurve* FindPlainRow(Name) const`·`const TangentCurve* FindTangentRow(Name) const`: 모드가 다르면 null이다.
  - UE는 모드가 다르면 Error 로그를 남기고 null을 돌려준다(`.h:150-170`). 행이 없으면 `bWarnIfNotFound`일 때 Warning이다.
  - Rocks 조회는 로그를 남기지 않는다. 오류 보고는 호출자가 결과로 한다(◇ 7.2절 9).
- `Result<float> TryEvaluateRow(Name row, float time) const`: 모드에 맞는 행을 `Evaluate(time)`(fallback 0)한다. 행이 없으면 NotFound다(None 이름 포함).
- `usize RowCount() const`, 행 이름을 삽입 순서로 도는 `ForEachRowName`.

#### 3.2.3 편집

UE `AddRichCurve`·`AddSimpleCurve`·`RemoveRow`·`DeleteRow`·`RenameRow`·`EmptyTable`(`CurveTable.cpp:527-620`)에 대응한다.

- `Result<TangentCurve*> TryAddTangentRow(Name row)`·`Result<PlainCurve*> TryAddPlainRow(Name row)`:
  - 모드가 Empty면 그 모드로 정한다.
  - 같은 이름 행이 있으면 빈 커브로 바꾼다. 자리는 유지된다.
  - 오류:
    - 다른 모드의 표면 TypeMismatch다(UE는 `check`로 프로세스를 멈춘다).
    - None 이름은 InvalidArgument다(UE는 이름 검사 없이 넣는다 — ★ 7.2절 3).
    - 할당 실패는 OutOfMemory이고 표는 그대로다.
  - 돌려준 포인터는 다음 표 편집까지 유효하다.
- `bool RemoveRow(Name row)`: 있으면 지우고 참이다. UE `RemoveRow`·`DeleteRow` 둘을 하나로 합쳤다.
- `Result<void> TryRenameRow(Name from, Name to)`는 아래 순서로 판정한다.
  1. 이름이 같으면 아무 일도 하지 않고 성공이다.
  2. `from`이 없으면 NotFound다(UE는 무시 — ◇ 7.2절 4).
  3. `to`가 None이면 InvalidArgument다.
  4. `to`가 이미 있으면 아무 일도 하지 않고 성공이다(UE와 같다).
  5. 그 밖이면 행을 지우고 새 이름으로 끝에 넣는다.
  - UE는 Add 뒤 Remove(`:601-611`)이고, sparse set이라 자리가 다를 수 있다(◇ 7.2절 12).
- `void EmptyTable()`: 모든 행을 지우고 모드를 Empty로 둔다(UE `EmptyTable`).
  - UE의 전역 캐시 무효화(`GlobalCachedCurveID` — GAS 캐시용)와 `OnCurveTableChanged`는 두지 않는다(1.2절 미룸).

### 3.3 `CurveTableRowRef`(UE `FCurveTableRowHandle`)

- `ROCKS_STRUCT() struct CurveTableRowRef { ROCKS_PROPERTY() ObjectRef<CurveTableAsset> table; ROCKS_PROPERTY() Name row; }`
  - UE의 `CurveTable`·`RowName`(`CurveTable.h:261-338`)에 대응한다.
  - 참조 struct이므로 둘 수 있는 자리는 3.1절과 같다.
- `bool IsNull() const`: 표가 null이고 행이 None이면 참이다(UE와 같다).
- `Result<float> TryEvaluate(ObjectRegistry&, const ObjectAccessContext&, float time) const`: 표를 풀어 `TryEvaluateRow(row, time)`를 부른다.
  - 표가 null이면 NotFound다.
  - 행이 없으면 NotFound다.
  - 표가 풀리지 않으면 Resolve 오류다.
  - UE의 두 경로를 하나로 했다.
    - `Eval(X, OutY, Context)`: 성공 bool을 돌려주고, 실패하면 out은 그대로다(`CurveTable.cpp:1134-1146`).
    - 블루프린트 `EvaluateCurveTableRow`: RowFound·RowNotFound를 돌려준다(`DataTableFunctionLibrary.cpp:20-36`).
- 같음은 리플렉션 값 비교다. UE `operator==`(포인터·이름)와 같다.
