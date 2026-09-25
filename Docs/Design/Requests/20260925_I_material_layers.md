# 요청: 재질 모델에 clearcoat와 박막 간섭 (I 트랙, 2026-09-25)

## 왜 필요한가

I는 이전 엔진의 데이터 월드 장면(`C:\Users\USER\Unravel\Assets\NativeEngine\DataWorld\NativeDataWorld.unity`)을 새 렌더러로 옮긴다.
그 장면의 재질 목록(`Materials.asset`, `TitanMaterialLibrary`)에서 **재질 1**을 모든 동적 상자와 캐릭터가 쓴다. 재질 1의 저작값은 다음과 같다.

| 필드 | 값 |
|---|---|
| BaseColor | (0.04, 0.32, 0.65) |
| Metallic / Roughness | 0.85 / 0.30 |
| **Clearcoat / ClearcoatRoughness** | **1 / 0.12** |
| **Surface.Flags = ThinFilm** | 기판 굴절률 1.5, 소광 2, 막 굴절률 1.4, **막 두께 420 nm**, 덮임 0.35 |

INTERFACES 8.1 재질 v1(GGX + Lambert, 다중 산란 보정, Foliage 투과)에는 clearcoat 층과 박막 간섭이 없다. 그대로 옮기면 코팅의 두 번째
스페큘러 lobe와 무지갯빛 간섭색이 사라진다. 그림이 달라지는 품질 손실이다.
`TitanMaterialLibrary` 저작 면에는 이 밖에 Anisotropy, ClothSheen·Roughness·Tint, 세부(detail) 층도 있다. 데이터 월드는 이 셋을 쓰지 않는다.

## 원하는 변경

INTERFACES 8.1에 재질 층을 추가하는 것이다. 코어가 인터페이스를, M이 실시간 커널을, C가 기준 경로추적기를 같은 식으로 구현한다.

1. **Clearcoat**: 세기 c ∈ [0,1], 코팅 지각 거칠기 r_c. 코팅은 굴절률 1.5인 GGX 유전체 lobe다. 아래 층은 코팅의 방향별 Fresnel 투과만큼 감쇠한다
   (에너지 보존: f = f_coat + (1 − c·F_coat(μ_o))(1 − c·F_coat(μ_i)) · f_base). 다중 산란 보정은 코팅 lobe에도 같은 E 표 방식으로 한다.
2. **박막 간섭**(선택, 데이터 월드는 사용): 막 굴절률 η_f, 두께 d(nm), 덮임 비율. 스페큘러 Fresnel을 Airy 합으로 바꾼다
   (Belcour & Barla 2017, 스펙트럼 → RGB 적분은 표로). 덮임 < 1이면 간섭 Fresnel과 원래 Fresnel을 덮임 비율로 섞는다.
3. 뒤로 미뤄도 되는 것: 비등방(anisotropy), sheen(Charlie), detail 층. 저작 도구가 쓰는 장면이 생기면 같은 절차로 요청한다.

- 장면 데이터(`scene::Material`, `.unxscene` v2)와 GPU 재질 레코드(80 B)에 필드가 늘어난다. 레코드 크기가 바뀌면 코어가 판단한다.
- ABI(`UnxMaterialDesc`)는 I가 같은 필드를 추가한다(ABI 버전 +1).

## 그때까지의 처리

브리지는 재질 1을 v1의 기저 층(BaseColor, Metallic 0.85, Roughness 0.30)으로 옮긴다. 빠지는 층은 재질마다 로그와 I 상태 문서에 기록한다.
감추지 않는다. 데이터 월드 측정에는 이 차이가 있다고 함께 적는다.

## 영향

- 코어: INTERFACES 8.1, `MaterialModel.h/.hlsli`, `scene::Material`, `.unxscene` 버전, `gpu::Material`.
- M: 셰이딩 커널의 재질 평가(층 추가, 셰이딩 비용에 lobe 하나 추가 [예상 ALU 소폭, 메모리 불변]).
- C: 기준 경로추적기 재질 샘플링(코팅 lobe 선택 확률).
- R: hit 셰이딩의 재질 평가(같은 함수 사용).
