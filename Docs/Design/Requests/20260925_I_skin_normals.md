# 요청: 스킨 법선을 관절 행렬의 여인수(cofactor)로 변환 (I 트랙, 2026-09-25)

## 왜 필요한가

`Passes/Common/Deformation.hlsli`의 `skin()`은 법선과 탄젠트를 모두 관절 행렬의 3×3(`(float3x3)m`)으로 변환한 뒤 정규화한다.
팔레트가 회전 + 균일 스케일이면 정확하다. 비균일 스케일이나 전단이 있는 관절에서는 법선이 틀린다. 정확한 변환은 역전치, 곧 여인수 행렬이다.

실제로 생기는 경우:
- **이전 엔진 데이터 월드의 캐릭터**: 비주얼 파트 스케일이 (0.6, 0.8, 0.6)이다. 렌더러 인스턴스 변환은 회전 + 균일 스케일만 허용한다(INTERFACES 6.1).
  그래서 브리지는 스킨 파트의 비균일 스케일을 관절에 접는다(jointToModel' = S × jointToModel). 위치는 정확하지만 법선은 S 쪽으로 기운다.
- 저작 애니메이션의 squash & stretch 관절 스케일도 같은 문제를 낸다.

## 원하는 변경

`skin()`에서 법선만 관절 3×3의 여인수로 변환한다.
```hlsl
// cofactor(A) = det(A) * inverse(A)^T; rows: cross products of A's columns. Exact for any invertible joint; for rotation +
// uniform scale it is a positive multiple of A, so the normalised result equals today's.
float3x3 cofactor3(float3x3 a) { ... }
sn += w[k] * mul(cofactor3((float3x3)m), n);
```
- 탄젠트는 지금처럼 `(float3x3)m`으로 변환한다(탄젠트는 벡터).
- 가중 합의 순서(여인수로 변환한 뒤 가중 합)는 선형 혼합 스킨의 표준 근사이며, 지금 법선에 쓰는 방식과 같다.
- 비용 [예상]: 관절마다 외적 3개(9 FMA × 2). 메시 셰이더의 가시 스킨 정점만 처리하므로(설계서 2.8, 4K 2 M 정점) 0.001 ms 수준이다.
- C의 기준 경로추적기가 스킨 메시를 쓸 때도 같은 식이어야 한다.

## 영향

- 코어: `Deformation.hlsli` 한 곳. V(래스터), S(그림자 페이지, V 서비스 경유), R(BLAS refit)이 같은 함수를 쓰므로 한 번에 반영된다.
- 회전 + 균일 스케일 팔레트에서는 결과가 바뀌지 않는다(정규화 뒤 같은 방향).

## 결과 (코어, v1.14)

- 반영: `cofactorNormal(float3x3 a, float3 n)` = 여인수(det · A⁻ᵀ) × sign(det). 거울 관절(det < 0)에서도 법선이 바깥쪽을 향하게 부호를 맞췄고, 방향은 역전치와 같다. `skin()`의 법선 가중 합이 이 함수를 쓴다. 탄젠트는 요청대로 `(float3x3)m`이다.
- 검증 [실측]: 단위 테스트 `skin_normals_use_the_cofactor`. GPU 결과를 CPU 역전치와 비교했다. 회전 × 균일 스케일, 호스트 파트 스케일(0.6, 0.8, 0.6), 전단, 거울 네 경우에서 성분 최대 차이는 5.96e-8이다. 단위 24/24, V 4/4, 디버그 레이어 오류 0.
