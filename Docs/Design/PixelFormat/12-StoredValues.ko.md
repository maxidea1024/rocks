## 12. 저장 값과 번호 재배정

형식 번호는 쿠킹 텍스처 메타에 저장된다(RenderBase 6.5절 "직렬화": 고정 번호표와 안정 이름, enum 순서 값 금지, 번호 재사용 금지). 이번에 번호를 다시 매겨도 되는지 확인하였다.

- `Assets/`에는 `Assets/Branding`(SVG·PNG·HTML)만 있다. 쿠킹 텍스처·패키지·에셋 너겟 값이 없다(`git ls-tree origin/feature/engine-runtime Assets`).
- 시험 fixture에 형식 값을 담은 바이너리가 없다. 형식을 쓰는 시험은 enum을 코드로 쓴다(3절 사용처).
- 형식 값을 파일에 쓰는 코드는 `prototype/texture-importer` 브랜치(6a2ffc5 "[Prototype] 텍스처 임포터·AssetRuntime 시제품(참고 보존, 병합하지 않음)")의 `TextureAsset.h:133`(`u32 format(graphics::Format)`)뿐이다. 이 브랜치는 병합하지 않으며 커밋된 쿠킹 파일도 없다.

따라서 **이번에 번호를 다시 매긴다.** 기존 값 1(`RGBA8Unorm`)·2(`RGBA8UnormSrgb`)는 새 번호 12·13이 된다. PF1 이후로 번호표는 추가만 한다. 직렬화는 `PixelFormatNumber`(바이너리)·`PixelFormatName`(텍스트·JSON)이며 와이어 폭은 AssetSystem A5 형식이 정한다(정점 형식처럼 u32 권장).

**번호의 뜻은 바꾸지 않는다(개정 6, 에셋 세션 요청 2026-11-25).** `pixelFormat` 숫자는 쿠킹 데이터(`TexturePlatformData`)에 그대로 저장된다. 따라서 기존 번호의 뜻을 바꾸거나 다른 뜻으로 다시 쓰지 않는다. 부득이 뜻을 바꾸면 `TexturePlatformData` 타입 버전을 올려 다시 쿠킹되게 한다.
