# 요청: 화면 프로브 조회의 구조 변경 (R 트랙, 2026-09-25, M 요청 20260925_M_shading_lookup_cost.md에 대한 답)

## 현재 상태 [실측]

R 게이트의 격리 벤치(`Passes/GI/Gates/ProbeLookupBench.hlsl`)로 쟀다. city_block 4K에서 모든 픽셀이 M과 같은 방식으로 조회를 부른다. 수치는 조회 없는 커널(0.096 ms) 대비 증분이다.

| 조회 | a28f3a7 전 | a28f3a7 후 | 내용 |
|---|---:|---:|---|
| 4 프로브 발자국 | 0.11 | 0.078 | 프로브당 1 Load(평면 0: 월드 위치 + 법선), 픽셀 역투영 1회 |
| `screenProbeIrradiance` | 0.253 | 0.193 | 발자국 + 가중 프로브마다 SH 27 fp16 |
| `screenProbeRadiance` (K) | 0.79 | 0.40 | 발자국 + 서로 다른 지도 블록마다 1~2 밉 소프트웨어 쌍선형(RGB9E5) |
| 둘 다 | 1.04 | 0.60 | 발자국을 두 번 계산 |

- 결과는 이전과 같다(주소·병합·밉 생략이 정확 동치임을 확인했다; GI·반사 해석 테스트 통과).
- 설계 2.11의 셰이딩 예산은 0.42 ms이고 M 몫이 0.37 ms라, 조회에 남는 것은 약 0.05 ms다. 지금 구조로는 닿지 않는다.
- K 경로 지도 읽기(≈0.32 ms)가 가장 크다. 셰이더 ALU로 하는 쌍선형과 RGB9E5 복원이 원인이다.

## 원하는 변경

1. **발자국 공유 조회**(R 함수 추가, M이 호출 교체):
   `ScreenProbeLighting screenProbeGather(ProbeSrvs, uint2 pixel, float3 worldPos, float3 normal, float linearDepth, float3 dir, float coneHalfAngle, bool back)`.
   반환은 `{ irradiance(n), irradiance(-n) (back일 때), occlusion, K radiance }`다.
   - 발자국을 한 번만 계산해 약 0.078 ms를 줄인다. Foliage는 두 번을 줄인다.
   - `worldPos`는 M이 이미 갖고 있으므로 픽셀 역투영을 없앤다.
   - 기존 두 함수는 이 함수를 감싼 형태로 남긴다.
2. **K 경로 지도를 하드웨어 필터 텍스처로**(코어 + M):
   - 새 `ViewResources::screenProbeMaps`: 생산 R, 소비 M(`SrvCompute`). SRV는 `ProbeSrvs.pad0`로 넘긴다.
   - 서로 다른 캐시 항목의 지도만 담는 아틀라스다. 밉 0/1/2를 각각 1텍셀 테두리가 있는 타일(10², 6², 4²)로 두어 가장자리 클램프와 정확히 같게 한다.
   - 서로 다른 블록마다 `SampleLevel` 2회로 끝난다. 쌍선형 4탭과 RGB9E5 복원 4회를 텍스처 유닛이 대신한다.
   - 형식 문제: R9G9B9E5_SHAREDEXP는 UAV 저장이 안 된다. 선택지는 둘이다.
     - (a) 코어의 `RenderGraph::createTexture`가 **캐스팅 가능한 형식 목록**(R32_UINT로 쓰고 R9G9B9E5_SHAREDEXP로 읽기, 같은 32비트)을 지원한다. 이 조합이 허용되는지 코어가 확인해 달라.
     - (b) RGBA16F 아틀라스(정밀도는 RGB9E5 이상, 8 B/텍셀). 크기는 서로 다른 소유 항목 수 × 152 텍셀 × 8 B다. 소유 항목 수는 R이 측정해 알린다.
3. 위 두 가지가 들어오면 R이 벤치와 M 게이트로 다시 잰다. 예산(0.05 ms)에 닿지 않으면 남는 항을 R이 다시 가른다(설계 가정 오류인지 구현 비효율인지).

## 영향

- 코어: `ViewResources` 필드 하나, (a)라면 createTexture의 castable 형식.
- M: `ShadeOpaque.hlsl`의 조회 호출 교체, 새 텍스처 선언.
- R: 함수 추가, 아틀라스 생산(GiProbeMaps가 소유 항목만 쓰므로 비용이 거의 늘지 않는다 [예상]).

## 결과 (코어, v1.13)

- **2) 형식은 (a)로 결정했다 [실측]**:
  - 코어가 `TextureDesc::srvFormat/uavFormat`(캐스팅 가능한 뷰 형식)를 지원한다.
  - 자원은 relaxed format casting 목록과 함께 만들고, SRV와 UAV는 각자 형식으로 만든다.
  - 단위 테스트 `graph_castable_view_formats`: R32_UINT UAV로 RGB9E5 비트를 쓰고 R9G9B9E5_SHAREDEXP SRV의 `Load`로 읽었다. 하드웨어 복원 값이 CPU 복원과 4,096 texel 모두 비트 단위로 같았다.
  - RGBA16F(8 B)의 절반 대역이다. 사용 예: `TextureDesc d{ "...", w, h, 1, 1, DXGI_FORMAT_R32_UINT }; d.srvFormat = DXGI_FORMAT_R9G9B9E5_SHAREDEXP;` UAV는 R32_UINT(`RWTexture2D<uint>`), SRV는 `Texture2D<float4>`로 `SampleLevel`한다.
- `ViewResources::screenProbeMaps`(생산 R, 소비 M `SrvCompute`)를 추가했다. 형식·배치는 R의 `ScreenProbes.hlsli`가 정한다.
- 1) `screenProbeGather`는 R의 공개 헤더 변경이다(5.6). R이 커밋하면 표를 고친다. M의 호출 교체는 M이 한다.
