# 요청: 장면 텍스처를 `gpu::Material`에 게시하는 경로 + 비등방 clamp 샘플러 (M 트랙, 2026-09-25)

## 왜

- `GpuScene::upload`(코어)는 재질의 텍스처 필드(`baseColorTexture` 등)를 전부 `kNone`으로 두고 "M의 텍스처 시스템이 채운다"고 적어 두었지만,
  M이 그 값을 `gpu::Material`에 넣을 인터페이스가 없다.
- 그 결과 **통합 프레임에서 V의 알파 테스트(`Passes/Visibility/AlphaTest.hlsli`, `m.baseColorTexture == UNX_NONE`이면 통과)가
  아무것도 잘라내지 않는다.** 잎·풀 카드가 사각형 그대로 그려지고, 그림자 페이지(깊이 래스터 서비스)도 같다. 숲 장면의 가시성·그림자·
  재질이 기준과 다르다.
- R의 hit 셰이딩도 베이스 컬러 텍스처가 없어 상수 알베도를 쓴다(GI·반사 색).
- M은 이미 텍스처 시스템을 갖고 있다(`Passes/Material/TextureSystem.cpp`, d8305bd): 모든 장면 텍스처의 전체 밉 체인을 올리고, 재질 해석이
  footprint 기울기로 바로 필터하는 형식으로 만든다. 지금은 M 내부 표(`MTextureSet`)로만 쓰고 있다.

## 원하는 변경

1. **트랙 진입점 `tracks::prepareScene(FramePassContext& fc)`** (`Tracks.h`, 구현 M).
   `FrameRenderer::record`가 **어떤 뷰의 프레임 상수도 할당하기 전에** 부른다. M은 여기서 장면 revision이 바뀌었으면 텍스처를 올리고 (2)로
   게시한다(로드 시 한 번, blocking). 꺼진 M의 빈 구현은 아무것도 하지 않는다(지금과 같음).
   - 프레임 상수 할당 뒤에 재질 버퍼를 바꾸면 그 프레임의 b1이 옛 SRV를 가리키므로, 반드시 할당 전이어야 한다.
2. **`GpuScene::setMaterialTextures(const std::vector<gpu::MaterialTextures>& perMaterial)`** (코어): 재질마다
   `{ baseColor, normal, roughMetal, emissive, occlusion }` bindless SRV(없으면 `kNone`)를 받아 재질 버퍼의 텍스처 필드를 채우고 버퍼를
   다시 올린다(이전 버퍼는 `deferRelease`). 재질 `revision`을 올린다(R 캐시 무효화: 알베도가 바뀐다). SRV의 수명은 M이 소유한다.
3. **게시되는 텍스처의 형식** (M 공개 헤더 `Passes/Material/MaterialTextures.hlsli`를 새로 두고 5.6 표에 추가):
   - `baseColorTexture`: RGBA8 sRGB, 전체 밉. 알파 테스트 재질이 쓰는 텍스처는 색 밉이 알파 가중(투명 텍셀 색이 새지 않음), **알파 밉은
     각 레벨에서 컷오프 통과 비율(텍스처의 coverage)을 레벨 0과 같게** 맞춘다(Castaño 2010). V의 `Sample(g_anisoWrap)` 알파 테스트가
     거리와 무관하게 같은 coverage를 본다. 레벨 0은 원본 그대로라 기준(mip 0 쌍선형)과 같다.
   - `normalTexture`: M의 기울기 모멘트(RGBA16_UNORM: 평균 기울기, 내부 분산, |평균|²; 범위 S는 재질별 값). 다른 트랙은
     `materialNormalMean(...)`(헤더 제공)으로 평균 법선만 읽거나 무시한다.
   - `roughMetalTexture`: RG8, `emissiveTexture`: RGBA8 sRGB 또는 RGBA16F — 장면 형식 그대로(밉 추가).
   - `occlusionTexture`: `kNone`(INTERFACES 8.1 v1에 사용이 정의되지 않음; C의 기준도 거부한다).
   - 텍스처별 wrap/clamp: `gpu::Material.pad0`에 비트(텍스처 5종 × 1 bit)로 넣는 것을 제안한다(크기 80 B 유지).
4. **정적 샘플러 s5 = anisotropic 16, clamp** (`Device.cpp` 루트 시그니처, `Bindless.hlsli`의 `g_anisoClamp`).
   장면 텍스처는 wrap/clamp 둘 다 있다(`scene::Texture::wrap`). 지금은 s3(aniso wrap)뿐이라 clamp 텍스처의 가장자리에서 반대편 텍셀이
   필터에 섞인다(발자국이 클수록 넓게). 샘플러 힙은 직접 인덱싱이지만 할당자가 없어 트랙이 만들 수 없다.

## 영향

- 코어: `Tracks.h` 진입점 1개와 빈 구현(Stubs/TrackM.cpp), `FrameRenderer::record`에서 호출 1줄, `GpuScene::setMaterialTextures`,
  `gpu::MaterialTextures`(20 B) 구조체, 루트 시그니처 정적 샘플러 1개, INTERFACES 5.2·5.6·6.3 문구.
- V: 코드 변경 없음(이미 `baseColorTexture`를 읽는다). 알파 테스트가 실제로 동작하기 시작한다 — 숲 장면의 가시성·그림자 결과가 바뀐다.
- R: 원하면 hit 셰이딩에서 `baseColorTexture`를 쓸 수 있다(선택).
- M: `TextureSystem`이 (2)로 게시하고, 재질 해석은 M 내부 표 대신 `gpu::Material`의 필드를 읽는다(내부 표 제거). 클램프 텍스처는 s5로.
- 테스트 프레임(코어·트랙 테스트)이 `FramePassContext`를 직접 만들 때도 `prepareScene`을 한 번 부르면 된다.
