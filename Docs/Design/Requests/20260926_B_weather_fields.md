# 렌더 B(S) 날씨·바람 장 인터페이스 공개 (B6, 2026-09-26)

조율 지시("날씨장과 바람장은 엔진 1·2가 쓰니 인터페이스를 일찍 공개한다")에 따라, 구현 전에 경계를 먼저 고정한다. 설계 근거는 FEATURES_GAME_KO.md 10절이다. 권위는 World에 있고, 렌더러는 그것을 읽어 캐시만 만든다.

## 1. 바람 `windAt(x, t)` — 공유 식 하나

- **권위**: World의 필드 레코드(`NW_FieldRecord`, quantity = `NW_FIELD_WIND`, NativeWorldFields.h)이고, 여기에 난류 성분을 더한다(아래 1.2).
  - 평가 순서와 연산은 `RuntimeCommon/FieldSampling.h`와 같다: 우선순위 오름차순, 그다음 WorldKey 순이다. ADD / REPLACE / MIN / MAX, 모양은 전역 / 단위 구 / 단위 상자, 틱 범위를 따른다.
- **공유 헤더**: UnravelNext `Native/Render/Passes/Atmosphere/WindField.hlsli`에 두고, C++에서도 같은 헤더를 `#include`한다(HLSL/C++ 공통 부분집합).
  - CPU 소비자(물리 천·로프·부력의 바람 항, VFX CPU 입자, 오디오 바람 소리)와 GPU 소비자(식생 변위, GPU 입자, 물 FFT 스펙트럼의 풍속·방향)가 **같은 식**을 쓴다.
  - float과 double의 차이만 남는다. CPU 결정성은 World 쪽 double 경로로 지킨다.
- **입력 레코드(GPU용, 64 B)**: `{ float3 origin(카메라 기준 상대), uint quantityOp(op | shape << 8 | flags << 16); float3x3 inverse basis(행 3개), float3 value; float4 turbulence(1.2) }`.
  - 호스트가 World 스냅샷에서 틱마다 채운다: `FrameContext::wind`(레코드 배열 + 개수 + 틱 시각 t). 순서는 World의 평가 순서 그대로다.

### 1.2 난류 성분 (`NW_COMPONENT_WIND_TURBULENCE`, 32 B — World 쪽 스키마 제안)

- `{ float amplitude (m/s), float lengthScale (m), float timeScale (s), uint octaves (1~4), uint seed, float3 pad }`. 바람 레코드에 붙는다.
- 값은 **curl noise**다: 스칼라 퍼텐셜 ψ 3개(시드별 3D gradient noise, 옥타브 합, 공간 스케일 = lengthScale, 시간은 4D로 timeScale)의 회전 ∇×ψ이다. 발산이 0이라 입자가 한곳에 모이지 않는다.
- 그 레코드의 모양 가중으로 곱해 더한다. 상자·구 경계에서 부드럽게 줄어드는 폭(falloff)은 World 스키마의 결정 사항이라 flags로 예약해 둔다.

### 1.3 렌더러 캐시

- S가 틱마다 64³ 격자(8 m 간격, 카메라 주변 512 m, RGBA16F, 2 MB)를 만든다(`windAt`의 격자 평가, 5~10 µs [예상]).
- `FrameResources::windField`(Texture3D SRV)와 HLSL `windSample(worldPos)`(삼선형)로 게시한다.
- **대역 조건**: 장의 대역 ≥ 16 m다. 더 고운 난류(입자·천)는 공유 헤더의 해석 경로 `windAt`을 직접 부른다.

## 2. 날씨 상태 — World 날씨 행 (틱 커밋)

`FrameContext::weather`는 호스트가 World의 날씨 행에서 채운다. 모든 모듈이 같은 값을 읽는다.

| 필드 | 뜻 | 소비자 |
|---|---|---|
| `rainRate` (mm/h) | 강우 강도 | FX 입자 수, 오디오, 물결 원천 |
| `wetness` W (0~1) | dW/dt = 강우 − W/τ_dry(World 적분) | M 젖음 = max(brick.wet, rainExposure × W) |
| `snowRate`, `snowDepth` S (m) | 강설과 쌓인 눈 높이 적분 | FX, M 눈 층(노출 × S − brick.snow_removed), V 지형 변위 |
| `fogDensity` (Mie 배율, 대수 궤적 값) | 전역 매질 | S 대기·프록셀(J_ms 재구축 경로, B4 열린 항목) |
| `cloudCover`, 날씨 맵 참조 | 구름(B5) | S 구름 |
| `lightning[]` {위치, 선분 끝, 에너지 J, 시작 틱} | 번개 사건 | 호스트가 1~2 프레임 광원으로 추가(S가 그림자 슬롯을 먼저 준다), FX 볼트, 오디오 |

## 3. 비 그림자 맵 (S)

- 위에서 내려다본 정사영 깊이 맵이다: 512², 카메라 주변 128 m, 비 방향(바람으로 기울기) 기준이다.
  - 캐스터는 대역 A 기하와 집합체 T다.
  - 장면이나 카메라가 셀 이상 움직일 때만 다시 그린다(≤ 1회/프레임, 0.05 ms [예상]).
- 게시: `FrameResources::rainShadow` + HLSL `rainExposure(worldPos)`(0~1, 비가 닿는 정도; 경계 2×2 PCF).
- 소비자: FX(빗방울 소멸·튀김 위치), M(젖음·웅덩이), 물결 원천(물 표면).

## 4. 안개 볼륨

- 전역 안개는 2절의 `fogDensity` → 대기 매질이다.
- 국소 안개(구·상자, 밀도·알베도·위상 g)는 프록셀 매질 목록 항목으로, 광원 목록과 같은 기계를 쓴다. 입력은 `FrameContext::fogVolumes`(레코드 배열)다.

## 5. 일정과 소유

- S(렌더 B): `WindField.hlsli`, 바람 캐시, 비 그림자 맵, 국소 안개 볼륨, 번개 그림자 우선순위, 날씨 → 대기 매질.
- World(엔진 1): 바람 레코드·난류 스키마 권위, 날씨 행과 적분, 스냅샷에서 GPU 레코드 채우기.
- VFX·FX(엔진 2): 입자와 볼트가 `windAt`, `rainExposure`를 소비한다.
- M(렌더 A): 젖음·눈 층이 `rainExposure`와 `weather`를 소비한다.
- 레코드와 필드 배치에 이견이 있으면 이 파일에 답을 적거나 SendMessage로 알려 달라. 합의 뒤 INTERFACES에 올린다.
