# 요청: Unity 디바이스·그래픽스 큐 위의 `Device` (I 트랙, 2026-09-25)

## 왜 필요한가 (실측)

호스트 경계(설계서 7.1-5)를 측정했다. `Native/Host/Gates/HostBoundary.cpp`, 원본 `Results/I/HostBoundary/standalone_*.json`, 요약은
`Docs/Status/I_STATUS_KO.md` 1절. 121패스 프레임(4K 2.6 ms)을 네 가지 경계로 돌렸다(600프레임 × 3회 교대, 중앙값).

| 경계 | 4K 프레임 주기 | 1440p | 리스트 하나로 융합한 경우 대비 |
|---|---|---|---|
| 호스트(=Unity) 큐에서 별도 리스트 | 2.699 ms | 1.021 ms | +0.04 ms (리스트 경계 2회, 각 17.4 µs) |
| 같은 디바이스의 자체 DIRECT 큐 + fence | 2.996 ms | 1.359 ms | +0.24~0.41 ms |
| 독립 디바이스 + 공유 텍스처·fence | 2.972 ms | 1.287 ms | +0.28~0.36 ms |

자체 큐는 4K 예산의 4~7 %를 이득 없이 쓴다. 그래서 **프레임은 Unity의 그래픽스 큐에서 리스트 하나로 실행한다.**
그 밖의 근거: 이전 엔진의 물리 GPU·VFX GPU가 Unity 디바이스에서 돌기 때문에, 같은 디바이스여야 그 출력을 복사 없이 읽는다.
Unity 6000.6 에디터·Player는 D3D12Core 1.618.1(SDK 618)을 싣는다 [실측]. UnravelNext(1.618.5)와 API·기능이 같다.

지금 `Device`는 디바이스를 직접 만들고 자기 큐 셋을 가진다. 한 프로세스에서 `D3D12CreateDevice`는 같은 어댑터의 기존 디바이스를
돌려주므로(싱글턴 [실측]) Unity 안에서도 Unity 디바이스에 붙기는 한다. 하지만 그래픽스 큐가 자기 것이라 위 표의 "자체 큐" 경로가 된다.

## 원하는 변경

1. **`DeviceOptions`에 외부 디바이스·큐**
   ```cpp
   // Host integration (I track, ARCHITECTURE 7.1-5): build on the host's device instead of creating one. The Device then
   // neither enables the debug layer nor enumerates adapters (the adapter comes from the device LUID for DeviceCaps).
   ID3D12Device* externalDevice = nullptr;
   // The host's DIRECT queue (Unity's). The Device's graphics Queue executes and signals on it (the fence stays the
   // Device's own), so a frame is one list on Unity's queue with no cross-queue synchronisation. Compute and copy queues
   // are still created on the device. The Device does not change this queue's priority.
   ID3D12CommandQueue* externalGraphicsQueue = nullptr;
   ```
   - 외부 큐의 `GetTimestampFrequency`는 그대로 쓴다. `Device::~Device()`의 `waitIdle`은 호스트가 디바이스를 닫기 전에 부른다(호스트가 순서를 지킨다).
   - 기능 검사(SM 6.6, 메시 셰이더, DXR 1.1, enhanced barriers, binding tier 3, heap tier 2)는 그대로 한다. 통과하지 않으면 실패한다.
2. **`Queue`의 실행 훅**
   ```cpp
   // Routes execute() through the host (Unity: IUnityGraphicsD3D12v8::ExecuteCommandList, which also declares the states
   // of Unity-owned textures the list touches to Unity's state tracker). signal()/waitGpu() stay direct on the queue.
   // Null (default) = ExecuteCommandLists on the queue.
   void setExecuteHook(std::function<void(ID3D12CommandList*)> hook);
   ```
   호스트는 렌더 이벤트(Unity 제출 스레드, 큐 접근 허용 이벤트) 동안만 훅을 설정한다: 출력 텍스처를 상태 `UNORDERED_ACCESS`로 선언하고
   `RenderGraph::importTexture(output, desc, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS)`로 가져온다.
3. **그 외 제출 지점**: 적재 때 제출(`GpuScene::upload`, TextureSystem, GI·반사·RayPipeline 초기화)은 그래픽스 큐를 쓰고 CPU로 기다린다.
   Unity 큐에서도 동작한다(큐 메서드는 스레드 안전, 우리 자원만 건드린다). 바꿀 필요는 없다. 다만 적재 제출이 프레임 제출과 겹치지 않도록
   호스트가 장면 적재를 렌더 이벤트 밖(메인 스레드, 프레임을 내기 전)에서 끝낸다.

## Unity 안 확인 [실측, Unity 6000.6 Player, `Results/I/HostBoundary/unity_player_20260925_093059.json`]

- 같은 작업을 Unity 디바이스에서 돌렸다. Unity 큐 리스트는 4K 2.763~2.813 ms, 1440p 1.036~1.041 ms다. 자체 HIGH 큐 + fence는
  4K 2.930~3.023 ms, 1440p 1.287~1.458 ms다(+0.16~0.42 ms). 결정은 그대로다.
- Unity 디바이스: SM 6.8, 메시 셰이더 tier 1, DXR 1.1, binding tier 3, heap tier 2, enhanced barriers 지원. `Device`의 기능 검사를 모두 통과한다.
- `D3D12CreateDevice`(FL 11_0, 12_2)는 Unity 디바이스를 돌려준다. 1의 `externalDevice`를 쓰지 않아도 디바이스는 같지만, 디버그 레이어를
  건드리지 않도록 명시적인 외부 디바이스 경로를 요청한다(디바이스가 있는 프로세스에서 디버그 레이어를 켜면 기존 디바이스가 제거된다).
- 순서 보존: Unity `ExecuteCommandList`로 낸 리스트와 같은 이벤트 안의 직접 큐 호출(ExecuteCommandLists, Signal, Wait)이 제출 순서대로
  실행됐다(모든 프레임 타임스탬프 순서 확인). 2의 훅(실행만 Unity 경로, signal은 직접)이 안전하다.
- Unity 큐는 NORMAL 우선순위이고 `ID3D12CommandQueue1::SetProcessPriority(HIGH)`는 DXGI_ERROR_UNSUPPORTED다. 그래서 1의 설명대로
  `Device`는 외부 큐의 우선순위를 바꾸려 하지 않는다. `DeviceOptions::queuePriority`는 compute·copy 큐에만 적용된다.

## 영향

- 코어: `DeviceOptions` 필드 2개, `Queue` 생성자 하나(외부 큐 감싸기)와 훅 하나. 기존 동작(둘 다 null)은 그대로다.
- 트랙 코드: 바뀌지 않는다.

## 결과 (코어, v1.9)

- 요청 그대로 반영했다(INTERFACES 4.1, 12절 v1.9). `DeviceOptions::externalDevice`는 디버그 레이어 요청과 함께 쓰면 실패하고, LUID로 어댑터를 찾으며, 기능 검사는 그대로 한다. `DeviceOptions::externalGraphicsQueue`는 자기 fence를 쓰고 이름·우선순위를 바꾸지 않는다. `Queue::setExecuteHook`는 `execute`만 훅으로 보내고 signal·wait는 직접 한다.
- 검증 [실측]: 단위 테스트 `device_on_host_device_and_queue`. 기존 디바이스와 별도로 만든 DIRECT 큐를 호스트로 삼았다. 같은 디바이스·같은 큐 확인, 훅 경유 제출 1회, fence 완료, 디버그 레이어 요청 거부를 확인했다. 단위 19/19, 디버그 레이어 오류 0.
- `GpuScene::flushUpdates`(v1.8)는 그래픽스 큐에 리스트 하나를 `Device::submit`으로 낸다. 그래서 훅이 켜져 있으면 Unity 경로로 나간다. 렌더 이벤트 안에서 `FrameRenderer::record`를 부르면 순서가 맞다.
