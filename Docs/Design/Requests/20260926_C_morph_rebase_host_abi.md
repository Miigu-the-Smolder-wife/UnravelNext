# 요청: 모프(C4)·원점 재설정(C9)의 호스트 ABI (렌더 C → A, 2026-09-26)

렌더 코어 쪽(GpuScene, Deformation.hlsli, V 컬링, FrameRenderer)은 C가 패치로 드린다(검증 뒤). 여기는 호스트(UnravelNext.dll ABI, Unity 변환기)가 할 일의 명세다.

## 1. 모프 입력 (씬 구성 시, UnxSceneCommit 전)

```c
// 메시의 블렌드 셰이프 하나: 오름차순 정점 번호 vertexCount개, 위치 오프셋 3 float씩, 법선 오프셋 3 float씩(없으면 null).
UNX_API int32_t UNX_CALL UnxSceneAddBlendShape(UnxRenderer r, uint32_t mesh, const char* name, uint32_t vertexCount,
                                               const uint32_t* vertices, const float* deltaPositions, const float* deltaNormals);
// 메시의 정점 애니메이션: frameCount × 정점 수의 위치(3 float씩, 프레임 우선), 법선(같은 배치, 없으면 null), 초당 프레임, 반복.
UNX_API int32_t UNX_CALL UnxSceneSetVertexAnimation(UnravelNext r, uint32_t mesh, float framesPerSecond, uint32_t frameCount, uint32_t loop,
                                                    const float* positions, const float* normals);
```
- scene::Mesh::blendShapes / vertexAnimation에 그대로 들어간다(Native/Scene, 1175fed). validate가 크기·오름차순·"VAT 메시는 스킨·셰이프 없음"을 검사한다.
- 인스턴스 초기값: scene::Instance::blendWeights(셰이프 수만큼), vertexAnimationTime — UnxSceneAddInstance의 확장 필드 또는 아래 프레임 함수의 첫 호출.

## 2. 프레임마다

```c
// 인스턴스마다 가중치(그 메시의 셰이프 수만큼, 연속 배치)와 VAT 시간. weightCount = 전체 합(호출 전 검사).
UNX_API int32_t UNX_CALL UnxFrameSetMorphs(UnxRenderer r, uint32_t count, const uint32_t* instances, const float* weights, uint64_t weightCount,
                                           const float* vertexAnimationTimes);
```
- HostRenderer: SetSkeletons와 같은 경로(FramePacket에 담고, 버린 패킷은 다음으로 넘김) → GpuScene::setMorph(frame, instance, weights, time).
- 모프 인스턴스는 업로드 때 정해진다(setInstances로 추가 불가: GpuScene이 거부) — 스킨 인스턴스와 같은 규칙.

## 3. Unity 변환기 (UnravelNextUnityContent)

- `Mesh.blendShapeCount`, `GetBlendShapeFrameVertices(shape, frameCount - 1, dv, dn, dt)`: 0이 아닌 정점만 모아 UnxSceneAddBlendShape(좌표 변환: z 반전, 스케일 굽기와 같은 규칙 — 오프셋은 벡터라 이동 없음, 법선 오프셋은 역전치 스케일).
- 가중치: `SkinnedMeshRenderer.GetBlendShapeWeight(i) / 100` 을 매 프레임 UnxFrameSetMorphs로(바뀐 인스턴스만 보내도 됨: 안 보내면 이전 값 유지 = 정착).
- 여러 프레임 셰이프(in-between)는 v1에서 마지막 프레임만(Unity의 선형 구간 보간은 v2) — 쓰는 에셋이 있으면 보고.

## 4. 원점 재설정 (C9)

```c
// 다음 프레임의 원점 이동(double, 축마다 1024 m 정수배). 이 프레임의 변환은 새 원점 기준으로 보낸다.
UNX_API int32_t UNX_CALL UnxFrameSetOriginShift(UnxRenderer r, const double shift[3]);
```
- HostRenderer: 패킷 적용 순서 = GpuScene::rebase(shift) → 변환 갱신 → FrameContext::originShift = shift. 카메라와 이전 카메라도 새 기준(이전 뷰는 FrameRenderer가 옮김).
- World(엔진 1)의 원점 선택과 한 틱에 맞춘다(엔진 1과 A가 직접: 요청 문서 20260926_C_origin_rebase.md 2절).
