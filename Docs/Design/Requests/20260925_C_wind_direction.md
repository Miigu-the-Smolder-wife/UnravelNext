# 요청: `windOffset`의 물체 공간 바람 방향 (C 트랙, 2026-09-25)

## 관찰
`Native/Render/Passes/Common/Deformation.hlsli`(코어 소유)의 v1 바람 모델:

```hlsl
const float3 dirObject = normalize(float3(dot(inst.objectToWorld[0].xyz, dirWorld),
                                          dot(inst.objectToWorld[1].xyz, dirWorld),
                                          dot(inst.objectToWorld[2].xyz, dirWorld)));
```

`objectToWorld[r]`는 물체→월드 3x4 행렬의 r행이므로 이 식은 `M · dirWorld`(물체→월드 회전을 월드 벡터에 한 번 더
적용)다. 월드→물체 변환은 전치 `Mᵀ · dirWorld`(= 열과의 내적)여야 한다. 지금 식으로는 월드 변위가 `R² · dirWorld`
방향이 되어, yaw θ로 놓인 수목은 바람 방향에서 2θ 돌아간 방향으로 흔들린다(인스턴스마다 방향이 제각각).

## 원하는 변경
```hlsl
const float3 dirObject = normalize(inst.objectToWorld[0].xyz * dirWorld.x
                                 + inst.objectToWorld[1].xyz * dirWorld.y
                                 + inst.objectToWorld[2].xyz * dirWorld.z);   // Mᵀ · dirWorld
```
(회전 + 균일 스케일이므로 정규화 후 R⁻¹ · dirWorld와 같다.)

## 영향
- V(래스터), S(그림자 페이지, V 서비스 경유), R(BLAS refit)이 같은 함수를 쓰므로 한 곳 수정으로 모두 바뀐다.
- C의 기준 경로추적기는 의도된 식(월드 변위가 바람 방향)으로 구현했다(`Reference/PathTracer/src/RtScene.cpp`
  `windOffset`). 고치기 전까지 바람이 있는 장면에서 엔진과 기준이 다르다. 바람이 없는 장면(속도 0)은 영향 없다.

## 처리 결과 (코어, 2026-09-25, INTERFACES v1.1)

- 지적이 맞다: `objectToWorld[r]`와의 내적은 `M · d`였다. `Deformation.hlsli`의 `windOffset`을 요청한 식(`Mᵀ · d`, 정규화)으로
  고쳤다. 래스터(V), 그림자 페이지(V 서비스), BLAS refit(R)이 모두 이 함수를 쓰므로 한 곳에서 바뀐다. 기준 경로추적기
  (`RtScene.cpp windOffset`)와 식이 같아졌다.
- 함께 추가: `windOffsetBound(inst, centre, radius)` — 구 안의 모든 점·모든 시각에 대한 |windOffset| 상한(흔들림 계수 ≤ 1,
  높이는 구 꼭대기). V의 컬링이 경계를 이만큼 키워 바람에 움직인 기하를 버리지 않게 한다. 모델이 바뀌면 둘을 같이 바꾼다.
