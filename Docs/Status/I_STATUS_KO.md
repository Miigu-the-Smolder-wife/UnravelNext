# I 트랙 상태 (통합: Unity 호스트 ↔ 새 렌더러) — 2026-09-25

표기: [실측] = 이 기계(i9-13900KF, RTX 4080, 드라이버 591.86, 모니터 2560×1440 143 Hz)에서 실행한 결과, [예상] = 비용식·가정.
빌드: `Tools/CI/Build.ps1 -Track I` (build/I, 트랙 V;M;S;R;I). 성능 측정은 `Tools/CI/GpuLock.ps1 -Track I` 아래에서만.
소유: UnravelNext `Native/Host/`, `Config/quality/host.toml`, 이 문서, `Results/I/` / 이전 저장소 `Assets/UnravelNextBridge/`.

## 1. 호스트 경계 결정 (설계서 7.1-5)

### 1.1 Unity가 싣는 런타임 [실측]

- Unity 6000.6.0f1 에디터(`Unity.exe`)와 Player(`WindowsPlayer.exe`, 빌드된 `NativeDataWorld.exe`)가 모두 `D3D12SDKVersion = 618`,
  `D3D12SDKPath = .\D3D12\`를 export한다(PE export 표를 읽어 확인). 싣는 `D3D12Core.dll`은 **1.618.1.0**(에디터 `Editor\D3D12`, Player 템플릿
  `AgilitySDK\x64` 둘 다). 시스템 `D3D12Core.dll`은 10.0.26100.9278.
- UnravelNext는 1.618.5다. SDK 버전(618)이 같으므로 API·기능 집합이 같다: enhanced barriers, SM 6.6(드라이버가 6.8 노출), 메시 셰이더,
  DXR 1.1, 독립 디바이스(`ID3D12DeviceFactory`), 큐 우선순위 변경(`ID3D12CommandQueue1::SetProcessPriority`)이 헤더에 있다.
  Unity 디바이스 위의 실제 기능 조회(SM·메시 셰이더 tier·큐 우선순위)는 Unity 안 프로브(1.3)가 기록한다.

### 1.2 독립 실행 게이트 [실측]

`Native/Host/Gates/HostBoundary.cpp` (`unx_gate_host_hostboundary`). 같은 프레임 작업(4K RGBA16F 왕복 패스 18 + 의존 1그룹 패스 102 +
RGB10A2 출력 UAV 패스 = 121패스, 패스 사이 전역 UAV 배리어)을 네 가지 경계로 돌린다. "호스트 큐"(DIRECT)가 Unity 큐 역할을 하고, 프레임 앞에
작은 패스 하나, 뒤에 출력 전체 복사(Unity의 표시 blit 역할)를 한다. 프레임 2개 in flight, 출력 2개 교대, 1.5 s 워밍업, 600프레임 × 3회(회차마다
순서 반대). 큐 우선순위 HIGH. 원본: `Results/I/HostBoundary/standalone_{4K,1440p}_*.json`.

| 경계 | 4K 프레임 주기 중앙값 (회차별) | 1440p (회차별) | 융합 대비 |
|---|---|---|---|
| fused_list: 호스트 작업 + 프레임 + 호스트 작업을 리스트 하나로 (하한) | 2.658 (2.663 / 2.654 / 2.650) ms | 0.980 (0.976 / 0.981 / 0.982) ms | — |
| host_queue_lists: 프레임을 호스트 큐의 별도 리스트로 (= Unity `ExecuteCommandList`) | 2.699 (2.706 / 2.694 / 2.696) | 1.021 (1.035 / 1.020 / 1.020) | **+0.04 ms** |
| own_queue: 같은 디바이스의 자체 DIRECT 큐 + fence 왕복 | 2.996 (2.991 / 3.057 / 2.892) | 1.359 (1.323 / 1.394 / 1.343) | **+0.24~0.41 ms** |
| independent_device: 독립 디바이스 + 공유 텍스처·공유 fence | 2.972 (2.979 / 2.974 / 2.938) | 1.287 (1.280 / 1.342 / 1.318) | **+0.28~0.36 ms** |

- host_queue_lists의 추가분은 리스트 경계 두 번이다: 프레임 앞뒤 GPU 틈 각 **17.4 µs**(P0b의 ExecuteCommandLists 경계 ~15 µs와 같은 크기).
- 별도 큐·디바이스에서는 프레임 자체가 느려진다(4K 작업 2.60 → 2.94~2.95 ms, 1440p 0.92 → 1.31~1.41 ms) [실측]. 호스트 큐의 fence 대기는
  작업 끝에서 **0.39~0.48 ms** 뒤에 풀린다(gapAfter) [실측]. 원인은 두 그래픽스 컨텍스트의 시분할로 보지만 확인하지 않았다.
  두 번째 큐의 겹침 이득은 없고 손해만 있다. 설계서 4.3(async 이득 2~6 %, 큐 간 왕복 62~79 µs)과 같은 방향이고 크기는 더 크다.
- 4K own_queue 2회차에 P95 48 ms / P99 111 ms 이탈이 있었다(다른 세션의 잠금 없는 정확성 실행과 겹친 것으로 본다, 원인 미확정). 중앙값 판정에는 영향 없음.
- 공유 텍스처 비용: 독립 디바이스의 출력(simultaneous-access, 공유 힙)은 호스트 쪽 복사가 0.060 vs 0.040 ms(4K)로 느렸다. 같은 디바이스
  (host_queue_lists, own_queue)는 Unity 텍스처에 직접 쓰므로 복사가 없다.
- 싱글턴 [실측]: 같은 프로세스에서 `D3D12CreateDevice`를 두 번 부르면 같은 디바이스가 온다(`deviceSingleton: true`). 독립 디바이스는
  `ID3D12DeviceFactory` + `DISALLOW_STORING_NEW_DEVICE_AS_SINGLETON`으로 만들어졌다.

### 1.3 Unity 안 프로브 [실측]

`UnravelNext.dll`(`Native/Host/src/Unity/UnityPlugin.cpp`, ABI 1)에 같은 작업의 Unity 안 판을 넣었다. Unity 디바이스에서 두 가지를 비교했다.
(a) Unity 큐의 리스트: `IUnityGraphicsD3D12v8::ExecuteCommandList`, 출력 상태 `UNORDERED_ACCESS` 선언.
(b) 자체 HIGH 큐 + fence 왕복.
Unity 큐에는 앞뒤로 타임스탬프 리스트를 넣었다. 프레임 구조는 before 이벤트 → 작업 이벤트 → Unity `Blit`(출력 → 1280×720 백버퍼) → after 이벤트다.
Player 빌드(Mono, 창 1280×720, vsync 끔), 4K·1440p, 600프레임 × 3회(회차마다 순서 반대), GpuLock. 원본: `Results/I/HostBoundary/unity_player_20260925_093059.json`.
Unity 쪽 코드는 `Assets/UnravelNextBridge`(이전 저장소 커밋 0125b960)이고, Player는 `Builds/UnravelNextProbe`다.

| Unity 디바이스 위의 경계 | 4K 프레임 주기 (회차별) | 작업 | 1440p 주기 (회차별) | 작업 |
|---|---|---|---|---|
| Unity 큐의 리스트 | 2.813 / 2.763 / 2.767 ms | 2.64~2.69 | 1.036 / 1.037 / 1.041 ms | 0.92 |
| 자체 HIGH 큐 + fence | 2.949 / 3.023 / 2.930 ms | 2.98~3.00 | 1.323 / 1.287 / 1.458 ms | 1.08~1.16 |

- 차이: 자체 큐가 **4K +0.16~0.26 ms, 1440p +0.25~0.42 ms** 느리다. 독립 실행과 같은 방향이다. 자체 큐에서는 작업이 느려지고(4K +0.34 ms),
  Unity 큐의 대기가 작업 끝에서 0.42~0.93 ms 뒤에 풀린다.
- Unity 큐 경로에서 리스트 경계의 GPU 틈은 11~12 µs다. 작업 끝부터 Unity blit·after까지는 0.04~0.06 ms다.
  Unity `deltaTime` 중앙값이 주기와 같아(2.79 ms) GPU 병목 상태에서 잰 값이다.
- **순서 보존**: Unity `ExecuteCommandList`로 낸 작업 리스트와 큐에 직접 낸 타임스탬프 리스트·Signal·Wait가 제출 순서대로 실행됐다
  (before < 작업 시작 < 작업 끝 < after, 모든 프레임).
- CPU: 작업 이벤트(121패스 기록 + 제출)는 Unity 제출 스레드에서 0.21~0.65 ms였다. 4K에서 커지는 것은 Unity `ExecuteCommandList` 안의 대기로 보인다(미확인).

사실 [실측, Player 안]:
- 로드된 런타임: `<Player>\D3D12\D3D12Core.dll` **1.618.1.0**.
- Unity 디바이스: SM **6.8**, 메시 셰이더 tier 1, DXR tier 1.1, binding tier 3, heap tier 2, enhanced barriers 지원, 추가 형식 typed UAV load 지원.
  UnravelNext `Device`의 필수 조건을 모두 만족한다. Unity `SystemInfo`는 "Direct3D 12 [level 12.2]", 스레딩 모드 `NativeGraphicsJobsSplitThreading`, `maxQueuedFrames` 2.
- `D3D12CreateDevice`(FL 11_0, 12_2 둘 다)는 **Unity의 디바이스를 돌려준다**(싱글턴).
- Unity 큐: DIRECT, 우선순위 **NORMAL**, 플래그 `DISABLE_GPU_TIMEOUT`. `ID3D12CommandQueue1`은 있지만 `SetProcessPriority(HIGH)`는
  **0x887A0004(DXGI_ERROR_UNSUPPORTED)** 로 거부됐다. 그래서 Unity 큐 경로는 NORMAL 우선순위로 돈다. P0b 실측(설계서 1.4)에 따르면 다른 GPU 앱이
  있을 때 이 차이가 P99를 늘린다. 이 영향은 whole-frame P95 게이트에서 따로 잰다. 자체 HIGH 큐로 피하면 프레임마다 0.16~0.42 ms를 더 내므로 그 방법은 쓰지 않는다.
- 추가 큐(DIRECT HIGH, COMPUTE, COPY)를 Unity 디바이스에 만들 수 있다. 독립 디바이스(`ID3D12DeviceFactory`)는 Unity 런타임 경로로는 만들어지고,
  플러그인 폴더(D3D12Core 미배치)로는 0x887E0003으로 실패한다.
- 스왑체인(Player): R8G8B8A8_UNORM, 3버퍼, FLIP_DISCARD, `ALLOW_TEARING | FRAME_LATENCY_WAITABLE_OBJECT | ALLOW_MODE_SWITCH`,
  용도 RENDER_TARGET_OUTPUT | SHADER_INPUT(UAV 없음), sync interval 0, present 플래그 ALLOW_TEARING.
  렌더러 출력(RGB10A2)은 Unity가 백버퍼로 옮긴다(blit). 백버퍼에 UAV로 직접 쓸 수는 없다.

### 1.4 결정 [실측 근거]

**Unity의 D3D12 디바이스를 공유하고, 프레임은 Unity 그래픽스 큐에서 리스트 하나로 실행한다.** 제출은 Unity 제출 스레드의 큐 접근 이벤트
(`kUnityD3D12GraphicsQueueAccess_Allow` + `FlushCommandBuffers`)에서 `ExecuteCommandList`로 하고, 출력 텍스처 상태를 선언한다.
전용 큐·전용 디바이스는 독립 실행에서 +0.24~0.41 ms, Unity 안에서 +0.16~0.42 ms를 더 쓰고 이득이 없다.
또 이전 엔진의 물리 GPU·VFX GPU(`TitanSharedGpu`)가 Unity 디바이스에서 돌기 때문에, 같은 디바이스여야 그 출력(입자·Matter 표면)을 복사 없이 읽는다.
Agility 1.618.5 기능은 1.618.1에서도 모두 있다(SDK 618).
필요한 코어 변경은 `Docs/Design/Requests/20260925_I_unity_queue_device.md`에 요청했다: 외부 디바이스·큐로 `Device`를 만들고, 큐 실행 훅을 둔다.

## 2. C ABI (`Native/Host/include/unx/host/UnravelNextHost.h`, ABI 5)

- 규칙: 앞에 `{size, version}`이 있는 고정 크기 구조체만 넘긴다. 크기는 헤더의 `static_assert`와 C#의 `RequireLayouts`가 같이 확인한다.
  반환은 결과 코드와 `UnxLastError()`다. C++ 객체는 넘기지 않는다.
- 렌더러: `UnxRendererCreate`(Unity 디바이스·큐, 또는 `UNX_RENDERER_STANDALONE`), `UnxRendererDestroy`.
- 장면 적재: 텍스처, 재질(INTERFACES 8.1), 메시(스킨 스트림 포함), 스켈레톤, 인스턴스, 광원, 환경(태양·대기·바람)을 `scene::Scene`으로 모은다.
  `UnxSceneCommit`이 검증하고, V의 클러스터 빌더를 돌리고, `GpuScene`에 올리고, `FrameRenderer`를 만든다.
  `UnxSceneContentHash`는 장면 identity다.
- 프레임: `UnxFrameSetTransforms`·`SetSkeleton`·`SetSkeletons`(ABI 3: 모든 스켈레톤을 호출 한 번에, 포즈를 목록 순서로 이어 붙인 버퍼)·
  `SetInstanceVisible`·`SetSun`(코어 v1.8 GpuScene 갱신)으로 다음 프레임에 쓸 값을 모은다. 애니메이션 재설계의 `na_present_batch`가
  `SetSkeletons`의 버퍼를 그대로 채운다(ANIMATION_DESIGN_KO.md 3.6.2, 7.2).
- 시간대·날씨(ABI 4): `UnxFrameSetEnvironment`가 다음 프레임부터의 태양과 대기를 바꾼다(태양만이면 `UnxFrameSetSun`). 제출 스레드가 프레임
  기록 전에 `GpuScene::source()`의 태양·대기를 고치고, 대기 트랙은 파라미터·태양이 바뀌면 LUT를 다시 만든다. 떨어진 패킷의 환경은 다음
  패킷으로 넘어가고 현재 장면 저장에도 들어간다. 바람은 커밋 뒤 바꾸면 거부한다: 변형 상한(INTERFACES 6.4)이 바람 고정을 전제한다.
  코어에 `Requests/20260925_I_wind_change.md`를 요청했고 v1.23으로 반영됐다. S의 페이지 규칙 뒤 바람 거부를 풀었다(프레임마다 바람도 바뀐다).
  정확성 시험(현재 장면 저장에 환경·바람 거부 추가)은 게임이 끝난 뒤 돌린다.
  `UnxFrameQueue`는 카메라·시각·출력 텍스처를 스냅숏으로 떠서 티켓을 돌려준다. `UNX_EVENT_RENDER`(Unity 제출 스레드)가 그 티켓의
  프레임을 `FrameRenderer`로 기록한다. 실행은 `Queue::setExecuteHook`(코어 v1.9)으로 Unity의 `ExecuteCommandList`에 보내며, 이때 출력의
  상태를 `UNORDERED_ACCESS`로 선언한다. 결과 조회는 `UnxFrameStatsLatest`다.
  렌더러 코드: `src/Renderer/HostRenderer.*`. ABI 변환: `src/Unity/RendererAbi.cpp`.
- 장면 저장 `UnxSceneSave`(ABI 3): 커밋 뒤에는 호스트가 지금 보이는 장면을 쓴다. 렌더된·큐에 있는·아직 큐에 넣지 않은 갱신을 순서대로 겹치고
  숨긴 인스턴스는 뺀다. 포즈는 패킷과 공유(`shared_ptr`)해서 복사하지 않는다. (처음 구현은 커밋 때의 내용을 써서 모든 인스턴스가 원점에 있었다.)
- 감김 검사: `UnxSceneAddMesh`는 삼각형의 99 % 이상이 자기 정점 법선과 반대로 감긴 메시를 거부한다(앞면은 렌더러 공간에서 반시계).
  좌표계 반전 내보내기가 순서를 안 바꾸는 종류의 실수를 적재 때 잡는다(2.2의 검은 화면 원인).
- **검증 [실측]** `unx_test_host_hostabi`(정확성 실행, 잠금 없음, `-Tracks "V;M;S;R;C;I"` 빌드):
  - 왕복: SceneGen 여섯 장면(도시 354 인스턴스, 숲 1.1 M 인스턴스, 수변, 실내, 야간 도시 광원 512)에 스킨 캐릭터를 하나 더했다.
    DLL의 export(LoadLibrary + GetProcAddress, Unity P/Invoke와 같은 입구)로 넘긴 뒤 렌더러의 `scene::contentHash`가 원본과 **모두 같다**.
    ABI는 장면 이름·seed·카메라·경로를 싣지 않으므로 그것을 뺀 해시를 비교한다.
  - 프레임: 실내 장면 1440p, 64프레임(정지 카메라)이다. ABI 경로와 `FrameRenderer` 직접 경로의 마지막 프레임이 **비트 단위로 같다**(동일 픽셀 100 %).
    두 경로 모두 셰이딩의 확률 항 두 개(GI 프로브, 반사; `shading.experiment_disable = 6`)를 뺀 시험용 품질 사본을 쓴다.
    이 항을 넣으면 직접 경로 두 번끼리도 P99 ~300/1023만큼 다르다(아래 R 참고).
    이 검사는 호스트의 프레임 준비(카메라, 시각, 출력, 페이싱)를 확인하는 것이고, 화질 검증은 각 트랙이 기준 영상으로 한다.
  - 현재 장면 저장: 커밋 뒤 렌더된 프레임(인스턴스 0 이동, 인스턴스 1 숨김, 포즈 A), 큐에만 있는 프레임(인스턴스 0 다시 이동),
    대기 중 갱신(`SetSkeletons`로 포즈 B, 태양)을 만든 뒤 저장·적재하면 최신 값이 **그대로** 나오고 숨긴 인스턴스는 빠진다.
    길이가 틀린 포즈 버퍼는 아무것도 기록하기 전에 거부된다.
  - 참고(R 트랙에 알릴 것): GI 캐시는 실행마다 결정적이지 않고 64프레임에서 아직 수렴하지 않는다(실내 천장의 얼룩, 직접 경로끼리 평균 차 33~50/1023).
    프레임당 갱신은 200k 항목 중 7.8k, 항목당 이력은 최대 32회다. 품질·시간 안정성 게이트의 대상이다.

## 2.1 Unity 쪽 (`Assets/UnravelNextBridge`, 이전 저장소)

- `Runtime`: P/Invoke(`UnravelNextNative`, `UnravelNextRendererNative` — 구조체는 blittable, 크기 검사), `UnravelNextRenderer`(관리 래퍼,
  Unity 왼손 → 렌더러 오른손: Z 반전), SRP 에셋·파이프라인(게임 카메라마다 프레임을 큐에 넣고 렌더 이벤트 → Unity blit), 호스트 경계 프로브.
  - 좌표 변환: 점·방향은 z → −z, 행렬은 F·M·F, 탄젠트 w는 부호 반전. **삼각형 순서는 (a, b, c) → (a, c, b)**: Unity 자료에서는
    cross(b − a, c − a)가 이미 바깥 법선 쪽이고, 반전(det −1)이 그 부호를 뒤집는다. 처음에는 "시계 → 반시계라 그대로"로 잘못 두어
    모든 면이 뒷면으로 셰이딩돼 검게 나왔다(2.2 시각 확인, `--describe`로 모든 메시의 감김이 법선과 100 % 반대임을 확인).
- `DataWorld`: `UnravelNextDataWorld`는 이전 데이터 월드(`NativeDataWorldHost`)를 새 렌더러로 보인다.
  - 읽는 것: 호스트의 시작 개체군, 레시피의 비주얼 파트, `TitanMaterialLibrary`. 비균일 스케일은 강체 메시에 굽고, 스킨은 관절에 접는다.
    스킨 메시는 Titan cooked 형식(`FoliageProduct`)에서 가져온다.
  - 매 프레임: committed World의 `WorldAffine`과 직전 tick을 렌더 시각으로 보간한다(회전 slerp, 이동 lerp). 스킨 포즈는 committed Animation
    포즈 원본(`AcquirePose`)에서 가져오고, 포즈 원본이 실제로 쓴 alpha를 루트에도 쓴다. 권위 상태에는 쓰지 않는다.
  - `UnravelNextFrameMeter`(명령줄로 켜는 측정), 편집기 도구 `UnravelNextDataWorldScene`(원본 장면은 두고 사본에 컴포넌트를 더해 저장, Player 빌드).
  - 아직 못 보이는 것(로그로 알림, 조용히 빼지 않음): 소프트 바디(tick마다 변형되는 정점), Matter, 파괴 조각, VFX(FX 트랙), cooked solid 비주얼, 텍스처 배열 층.
  - 데이터 월드 캐릭터 [실측, cooked 파일 파싱]: 정점 550개, 삼각형 832개, 정점당 가중치 1개, 뼈 2개, morph 없음(형식 v7). 손실 없이 옮겨진다.
- C# 오프라인 컴파일 검사: Unity의 Roslyn(`DotNetSdk/.../csc.dll`)으로 프로젝트의 컴파일된 스크립트 어셈블리를 참조해 브리지 네 어셈블리를 컴파일한다.
  Unity 시간을 잡기 전에 컴파일 오류를 잡는다.
- 배포: `Native/Host/Deploy.ps1`이 DLL, 커널, 품질 파일을 `Native~`에 놓는다. DLL이 로드돼 있으면 거부한다.
  빌드 identity는 링크가 성공한 뒤 DLL 옆에 쓴 사본에서 읽는다. Player 빌드 후처리는 커널과 품질 파일을 `<Game>_Data/UnravelNext`로 복사한다.

## 2.2 데이터 월드를 새 렌더러로 (Player) [실측]

**데이터 월드가 Unity Player에서 새 렌더러로 뜬다.** 장면은 원본 `NativeDataWorld.unity`의 사본이다(`Assets/UnravelNextBridge/DataWorld/
NativeDataWorld_UnravelNext.unity`, 원본은 그대로). Player는 `Builds/UnravelNextDataWorld`(Mono, 창 1280×720, vsync 끔)이다.
프레임은 Unity D3D12 큐에서 리스트 하나로 실행된다. 측정은 GpuLock, 1.5 s 워밍업, 해상도마다 서로 다른 완료 프레임 600개다.
원본: `Results/I/DataWorld/player_20260925_102722.json`(측정기의 형식 버그로 `max`는 잃었고 null로 복구했다. 다른 값은 원래대로다).

- 장면: 인스턴스 22(강체 상자·바닥·경첩·부표, 스킨 캐릭터, 영역 스트리밍 개체), 메시 10, 재질 6, 삼각형 2,584, 클러스터 89.
- 표시하지 못한 것(로그로 알림): 재질 1의 clearcoat·박막(요청 대기), 재질 2의 unlit(발광만으로 옮김), 캐릭터 법선(비균일 스케일, 요청 대기),
  소프트 바디·Matter·파괴 조각·VFX(렌더러 경로 없음).

| | GPU 프레임 중앙값 | P95 | P99 | CPU 기록 / 제출 (Unity 제출 스레드) | Unity 프레임 주기 중앙값 / P95 |
|---|---|---|---|---|---|
| 4K | **7.25 ms** | 8.14 | 10.29 | 0.26 / 0.06 ms | 12.30 / 18.28 ms |
| 1440p | **3.84 ms** | 4.53 | 4.82 | 0.30 / 0.07 ms | 5.49 / 16.67 ms |

- 트랙별 패스 중앙값 합(4K): **S 3.92**(`s.froxel.integrate` **2.76**, 가시성 0.36, 반영 0.15, VSM 무효화 0.13, markair 0.13, 페이지 래스터 0.12),
  **R 1.56**(반사 trace 0.76, 분류 0.26, GI 합 ~0.4), **M 1.21**(shade 0.69, resolve 0.29, edge 0.23), **V 0.17**. 합 6.87 ms, 나머지 ~0.4 ms는 배리어·틈이다.
  1440p: S 2.07(froxel 1.27), R 0.86, M 0.62, V 0.13.
- **주의(11:30 시각 확인으로 발견):** 이 실행을 포함해 2.2의 모든 데이터 월드 영상은 감김 결함(2.1) 때문에 모든 면이 뒷면으로 셰이딩되어
  빛 받는 면이 (0,0,0)이었다. 하늘만 맞다. 패스는 모든 픽셀에서 돌았으므로 비용 구성은 참고가 되지만, 올바른 영상의 측정이 아니다.
  고친 뒤 다시 잰다.
- 판정(결함 발견 전): 4K 7.25 ms는 목표 6.06 ms를 넘는다. 이 장면은 삼각형 2.6k로 거의 비어 있으므로 초과분은 부하가 아니라 트랙 패스의 고정비다.
  가장 큰 항은 프록셀 적분(설계 비용식 0.03 ms 대비 2.76 ms)이고 S 트랙에 알렸다.
  호스트 경계의 비용은 따로 쟀다(프로브, 1.3절: Unity 큐 리스트 경계 11~17 µs × 2).
- Unity 프레임 주기가 렌더러 GPU 시간보다 길다(4K 12.3 대 7.25 ms). 1440p P95는 16.7 ms로 tick 프레임에서 튀는 모양이다.
  이것은 데이터 월드 시뮬레이션(CPU tick, VFX GPU 등) 쪽 비용이라 감사 세션에 전달했다.

### 2.2.0 감김 수정 뒤 측정 (13:20~13:27) [실측, 유효]

DLL: UnravelNext 커밋 빌드 51e331b(ABI 4, 다른 트랙의 미커밋 변경 없음), 게임 종료 뒤, GpuLock(C의 기준 렌더는 잠금 중 멈춘다).
영상: 바닥·상자·캐릭터·바퀴가 빛을 받고 접촉 그림자가 있다(`player_20260925_132035_3840x2160.png`, 편집기 캡처도 같다).
금속 재질(metallic 0.85, roughness 0.3)에 어두운 반점·얼룩이 있다. **원인 분리[실측, 14:17~14:26, DLL 1371ac5(R의 GI 캐시 모서리 수정 뒤),
원해상도 캡처]:** 기본은 반점이 남는다. `shading.experiment_disable=4`(반사 G/M과 K 경로 스펙큘러 입사 끔)면 사라지고, `=2`(프로브 확산
조도 끔)면 그대로다. 그래서 R의 반사 결과(또는 경로 전환) 쪽이다. 네모 블록 모양이고 바닥 접촉부에서 짙다. R·M에 보냈다.
`player_cap_{default,disable4,disable2}_*_3840x2160.png`. Player 옵션 `-unxCaptureFull`(원해상도), `-unxQualityDir <폴더>`(품질 사본)를 더했다.
M의 추가 분리[실측, 14:34~14:36, 같은 조건]: `reflection.cache_lobe_half_angle_min_deg=0`(모두 K 경로, 광선 없음)은 반점이 없고, `=90`(모두 광선)은
반점이 남는다(거친 바닥의 firefly는 광선 강제 때의 1표본 잡음, 진단용). 그래서 R의 광선 hit 경로다. R은 광선 hit 발자국이 그 점의 모든 셀보다
거칠면 조회가 0을 돌려주던 결함을 고쳤다(0ff332b: 더 고운 레벨까지 찾는다). `player_cap_lobe{0,90}_*_3840x2160.png`.
빌드된 Player의 렌더러 파일(DLL·커널·품질)은 Unity 재빌드 없이 커밋 빌드로 바꿀 수 있다(스크래치패드 `update_player_renderer.ps1`, 관리 코드와
ABI가 그대로일 때만; `<Data>/UnravelNext/renderer.build.json`에 커밋을 적는다).

| 실행 | 해상도 | GPU 프레임 중앙값 | P95 / P99 | CPU 기록 / 제출 | 호스트 동기화(메인) | Unity 프레임 주기 중앙값 / P95 |
|---|---|---|---|---|---|---|
| Player | 4K | **5.05 ms** | 5.39 / 5.57 | 0.26 / 0.05 ms | 0.048 ms | 6.15 / 13.9 ms |
| Player | 1440p | **2.82 ms** | 3.34 / 3.60 | 0.28 / 0.05 ms | 0.049 ms | 3.93 / 17.4 ms |
| 독립 실행(같은 장면, `unx_gate_host_hostscene`) | 4K | 4.83 ms | 5.36 / 5.57 | 0.43 / 0.10 ms | - | - |
| 독립 실행 | 1440p | 2.75 ms | 3.22 / 3.49 | 0.34 / 0.08 ms | - | - |
| 편집기 Play 모드 | 4K | 5.57 ms | 10.16 / 11.25 | 0.35 / 0.07 ms | 0.081 ms | 16.0 / 57.5 ms |
| 편집기 Play 모드 | 1440p | 3.03 ms | 3.86 / 5.67 | 0.32 / 0.07 ms | 0.073 ms | 12.1 / 21.5 ms |

- 원본: `Results/I/DataWorld/player_20260925_132035.json`(+ PNG, 내보낸 장면 `data_world.unxscene`), `editor_20260925_132428.json`(+ PNG),
  `Results/I/HostScene/I host scene data_world {4K,1440p}_*.json`.
- 트랙 합(Player 4K, 프레임당): M 2.12(`m.shade` 1.80), S 1.23(`s.froxel.integrate` 0.46, `s.shadow.visibility` 0.33), R 1.07(`r.refl.trace` 0.33,
  `r.gi.trace` 0.21), V 0.17. 1440p: M 0.98, R 0.88, S 0.64, V 0.13.
- 판정: 데이터 월드가 Unity Player에서 **4K 5.05 ms, 1440p 2.82 ms**로 목표(6.06 / 5.00 ms) 안이다. Unity 프레임 주기 중앙값도 4K 6.15 ms다.
  단, 재질 1의 clearcoat·박막은 아직 빠져 있다(코어 재질 v2 대기).
- Unity 호스팅 비용: 같은 장면을 Unity 밖에서 그리면 4K 4.83, 1440p 2.75 ms다. 차이(+0.22 / +0.07 ms)는 패스별로 보면 호스팅이 아니라
  내용 차이다. Player 장면은 움직이고(떨어지는 상자, 애니메이션) 내보낸 장면은 정지라 VSM 래스터가 +0.08 ms 더 들고, `m.shade`는 오히려
  Player가 0.2 ms 적다(GI·장면 상태). 리스트 경계 비용은 GPU 타임스탬프 밖이며 프로브로 따로 쟀다(1.3절: 11~17 µs × 2).
- 편집기는 개발 환경이다. GPU 중앙값은 Player와 비슷하지만(4K +0.5 ms) 편집기 자체 그리기 때문에 꼬리와 프레임 주기가 크다.

### 2.2.1 재측정 11:23~11:34 (무효: 감김 결함 + CPU 오염)

S의 공기 볼륨 커밋(f1f6f8a) 뒤의 DLL(792f315 + 다른 트랙의 미커밋 변경)로 Player와 편집기 Play 모드를 쟀다.
원본: `Results/I/DataWorld/player_20260925_112345.json`(+ PNG 두 장, 내보낸 장면 `data_world.unxscene`), `editor_20260925_112414.json`.
- 영상: 빛 받는 면이 모두 (0,0,0)이다(감김 결함). 이 캡처로 결함을 찾았다.
- 오염: 11:17:51부터 `unx_reference.exe`가 잠금 없이 CPU 약 17코어를 썼다(8.5분에 CPU 8,700 s). Unity 프레임 주기가 Player 4K P95 363 ms,
  편집기 4K 중앙값 146 ms로 무너졌고 GPU 꼬리도 커졌다(Player 4K P99 54 ms). 조율 세션이 C에 잠금 중 기준 렌더 워커를 멈추도록 요청했다.
- 참고로 남기는 GPU 중앙값[실측, 위 두 조건 아래]: Player 4K 5.60 / 1440p 4.11 ms(`s.froxel.integrate` 0.44 ms, 이전 2.76),
  편집기 4K 5.95 / 1440p 2.85 ms. 호스트 동기화(Mono, 스킨 5체 + 강체 17개): 0.08 ms, 그중 `AcquirePose` 0.05 ms, 관절 변환+호출 0.006 ms.
- 내보낸 장면은 인스턴스가 모두 원점이었다(`UnxSceneSave` 결함, 2절에서 고침). 고친 브리지는 첫 동기화 뒤에 저장한다.

### 2.2.2 Unity 안의 설계 규모 부하 (`-unxScale 1024,256`, 13:23) [실측]

실제 개체의 committed 루트·포즈를 읽는 인스턴스를 더해 강체 1,024 + 스킨 256을 만든다(Player, Mono). 원본 `player_scale_20260925_132306.json`.

| 해상도 | GPU 중앙값 (P95) | 호스트 동기화 중앙값 (P95, P99) | 그중 `AcquirePose` | 관절 변환 + `SetSkeletons` 한 번 | Unity 주기 중앙값 |
|---|---|---|---|---|---|
| 4K | 6.51 ms (7.02) | 0.735 ms (1.39, 1.80) | 0.333 ms | 0.055 ms | 13.4 ms |
| 1440p | 3.56 ms (3.99) | 0.705 ms (1.26, 1.49) | 0.315 ms | 0.052 ms | 5.04 ms |

- 루트(1,280개 World 모음·보간·변환·`SetTransforms`): 약 0.35 ms = 개당 0.27 µs(Mono).
- 포즈: `AcquirePose`(Animation의 보간 평가·접촉·물리 오버레이)가 캐릭터당 1.3 µs(뼈 2개 캐릭터). 관절 변환은 관절당 약 0.1 µs다.
  설계 캐릭터(64본 × 256체 = 16k 관절)에서는 관리 코드 변환만 약 1.6 ms가 된다[예상, 위 실측으로 계산]. 애니메이션 재설계의
  `na_present_batch`가 `SetSkeletons` 버퍼를 네이티브에서 채우므로 이 관절 단위 관리 작업은 없어진다(프레임당 호출 2번).
- 루트도 같은 방식으로 옮길 수 있다: World의 `nw_snapshot_read_components`로 모은 두 tick의 `WorldAffine`을 네이티브에서 보간·변환한다.
  지금 0.35 ms라 메인 스레드 1 ms 할당 안이지만, 인스턴스가 늘면 비례해서 는다.
- **4K Unity 프레임 주기 13.4 ms의 원인[실측]: 데이터 월드 tick.** 측정기에 Unity `FrameTimingManager`(메인·렌더 스레드, GPU, present
  대기; 빌드 때만 Frame Timing Stats를 켠다)와 고정 스텝 시간(FixedUpdate 맨 앞·맨 뒤 훅)을 넣어 다시 쟀다(13:50, 13:55).
  4K: 메인 스레드 12.96 ms 중앙값(주기 12.98), 렌더 스레드 1.41, Unity GPU 프레임 7.02(렌더러 6.44), present 대기 0.007 ms.
  **고정 스텝(= `NativeDataWorldHost.FixedUpdate` tick)이 스텝당 12.57 ms**(P95 18.5, 최대 23.0, 556 스텝, fixedDeltaTime 0.02 s)다.
  4K는 GPU 쪽 프레임이 7 ms라 61 %의 프레임에 tick이 들어가 중앙값이 tick 프레임이 된다. 1440p는 메인 스레드 중앙값 1.49 ms,
  P95 13.9 ms(tick 프레임)이고 present 대기 2.75 ms로 GPU 쪽이 경계다. GC(Gen0)는 프레임당 약 1회(4K 10.2 s에 843회)다.
  렌더러(GPU, 렌더 스레드, 내 동기화 0.74 ms)가 원인이 아니다. tick 비용은 감사 세션 영역이라 전달했다. 원본 `player_scale_20260925_135022.json`,
  `player_scale_20260925_135522_4K_partial.json`(아래 TDR 때문에 로그에서 되살린 4K 창).
- **TDR (13:56:45):** 두 번째 실행에서 4K 창과 캡처가 끝나고 1440p로 바꾸는 순간 Player의 D3D12 장치가 제거됐다(887a0005, 이유 887a0006
  DEVICE_HUNG). 잠금은 I만 잡고 있었다. 먼저 알아챈 곳은 tick 안의 NativeVfx(`PrepareCohortBatch`: DeviceFailed)이고 원인은 아직 모른다.
  같은 DLL로 13:23에는 전환이 정상이었다. 규칙대로 GPU 실행을 멈추고 조율 세션에 알렸다. 덤프 `Crash_2026-09-25_045643944`.
  - 드라이버 이벤트(nvlddmkm 153, 13:56:38~43)가 이 실행 안에 있다. 유력한 원인[M의 GBV 증거, 코어 확인 대기]: RenderGraph의 가져온 리소스
    뷰 캐시가 raw 포인터를 키로 쓰고 지우지 않는다. 4K→1440p 전환에서 Unity가 출력 RT를 다시 만들 때 새 RT가 옛 주소를 받으면,
    파괴된 텍스처의 서술자로 쓴다(주소 재사용에 달려 간헐적). 옛 VFX 충돌 커널(FP64, 입자 × 표면 6,912)도 후보였다.
  - 대응: 새 정확성 시험 `unx_test_host_hostswitch`(HostRenderer 하나, 출력 4K↔1440p 네 번, debug layer + GBV)는 37a69d6에서 오류 0으로
    통과했다(83 s). 코어 수정(4532054: 가져온 뷰 캐시가 항목이 살아 있는 동안 리소스 참조를 쥐고, 설명이 바뀌면 뷰를 다시 만들고, 그 프레임에
    가져오지 않은 항목은 GPU 뒤에 버린다) 뒤 c4a1d2b에서 같은 크기 출력을 매 프레임 새로 만드는 단계를 더해 GpuLock 아래 다시 돌렸다.
    오류 0, 장치 제거 없음(130 s). 단, 수정된 캐시가 참조를 쥐므로 재생성 단계에서 주소 재사용은 16번 중 0번이었다. 이 시험은 새 계약과 맞다는
    것이지 옛 결함의 재현을 보인 것이 아니다(그 증거는 M의 GBV 오류와 코어 회귀 시험).
  - 임시 규칙(조율 세션): 코어 수정 전까지 Player·편집기는 해상도마다 따로 띄운다(한 프로세스 안 전환 금지). 하드웨어 GPU를 쓰는 모든
    실행은 GpuLock -Track I 안에서 하나씩 한다. Player 규모 실행은 옛 VFX를 CPU로 돌린다(`-unxVfxCpu` → `EnableVfxGpu = false`, 측정 조건에 적는다).
  - 짧은 규모 실행(14:13, DLL 37a69d6, `-unxVfxCpu`, 해상도마다 120프레임): 4K와 1440p 모두 통과했고 장치 오류는 없다.
    `player_scale_short_20260925_141334.json`. 4K GPU 8.11 ms 중앙값(짧은 창이라 P95 19.5), 고정 스텝 8.39 ms(옛 VFX CPU 실행이라 성능 근거는 아니다).
- GPU: 4K 6.51 ms는 목표 6.06 ms를 넘는다. 가장 큰 항은 `m.shade` 2.38 ms(4K). 캐릭터 256체와 상자 1,024개가 화면을 많이 덮는다.

## 2.5 설계 동적 규모의 호스트 경로 [실측]

`unx_gate_host_hostdynamic`(독립 실행, `UnravelNext.dll`의 export 경유, GpuLock): 강체 1,024개가 매 프레임 움직이고, 캐릭터 256체
(64본, 60k 삼각형 공유 메시, 정점당 가중치 2개)가 매 프레임 새 포즈를 받는다(RPP-1 N_dyn과 ARCHITECTURE 2.8 부하). 인스턴스 삼각형 1,540만.
원본: `Results/I/HostDynamic/host_dynamic_20260925_111846.json`, `..._112359.json`(패스별 분포 포함).

| | GPU 프레임 중앙값 | P95 | 호스트 갱신 호출(변환 1,024 + 포즈 256 + 큐) | CPU 기록 / 제출 |
|---|---|---|---|---|
| 4K (11:23, 프록시 전) | 8.27 ms | 10.77 | 0.140 ms | 0.69 / 0.08 ms |
| 1440p (11:23) | 5.79 ms | 6.51 | 0.134 ms | 0.69 / 0.08 ms |
| **4K (13:04, d3701c7 커밋 빌드)** | **4.69 ms** | 5.26 (P99 5.56, 최대 6.15) | 0.165 ms | 0.80 / 0.09 ms |
| **1440p (13:04)** | **3.08 ms** | 3.55 (P99 3.78) | 0.143 ms | 0.70 / 0.07 ms |

13:04 실행[실측]: 커밋 기준 빌드(`-Track all -Committed`, 다른 트랙의 작업 중 파일 없음), 게임 종료 뒤, 첫 프레임 제외 수정 뒤, 호스트는
`UnxFrameSetSkeletons` 한 번. 원본 `Results/I/HostDynamic/host_dynamic_20260925_130428.json`.
- R 캐릭터 프록시(2c10e34): RayScene "deformed 763,392 tris (0 above proxy budget)" = 256 × 2,982. `r.as.refit` 1.61 → **0.355 ms**,
  `r.as.deform` 0.26 → 0.034 ms(4K). R 예상 약 0.5 ms보다 작다. 설계 2.8 할당 0.13 ms(근거리 64체만 refit, 군중 192체는 캡슐)와는 아직
  2.7배다. 지금은 256체 모두 프록시를 refit한다(체당 1.39 µs). 군중 캡슐 판단은 R 몫이다.
- S VSM 변위 경계(dc6d35d): `s.vsm.raster.raster` 0.81 → **0.16 ms**, `s.shadow.visibility` 0.36 → 0.14 ms(4K).
- M: `m.shade` 1.32 → 0.78, `m.resolve` 0.38 → 0.24 ms(4K).
- 트랙 합(4K, 프레임당): R 1.63, S 1.51, M 1.12, V 0.17. 큰 항: `m.shade` 0.78, `s.froxel.integrate` 0.55, `r.refl.trace` 0.36,
  `r.as.refit` 0.36, `r.gi.trace` 0.29, `m.resolve` 0.24.
- 판정: 이 부하(설계 동적 규모, 인스턴스 삼각형 1,540만)에서 4K 4.69 ms, 1440p 3.08 ms로 목표(6.06 / 5.00 ms) 안이다. 장면에 수목·물·
  광원·VFX가 없으므로 RPP-1 전체 부하의 판정은 아니다.

- 큰 항(4K): `r.as.refit` **1.61 ms**(캐릭터 256 × 60k 원본 refit. RayScene이 "256 above proxy budget"을 보고한다. V의 클러스터 LOD 절단이
  생기면 8k 프록시로 간다, 설계 0.13 ms), `m.shade` 1.32, `s.vsm.raster.raster` 0.81, `s.froxel.integrate` 0.45, `m.resolve` 0.38,
  `r.refl.trace` 0.38, `s.shadow.visibility` 0.36, `r.gi.trace` 0.26, `r.as.deform` 0.26. 조율 세션을 거쳐 R·V·S·M에 전달했다.
- 게이트 결함(고침): 첫 실행의 4K 창에 렌더러의 첫 프레임이 들어갔다(`s.atmosphere.multiscatter` 23.3 ms 1회, 최대 45 ms).
  첫 프레임은 파이프라인 생성 때문에 1.5 s 워밍업이 끝난 뒤에 완료 보고가 온다. 이제 워밍업이 끝난 뒤 큐에 넣은 프레임만 센다
  (게이트와 Unity 측정기 모두). 이 LUT 최초 생성 23 ms 자체는 태양 변화 때 멈춤이 되므로 S에 전달했다.
- P95 10.8 ms(4K)는 같은 시각 잠금 없이 돈 기준 렌더러의 영향이 섞였을 수 있다. 오염 없는 조건에서 다시 잰다.
- 호스트 호출 비용은 스켈레톤마다 호출하던 때의 값이다. `UnxFrameSetSkeletons`(한 번)로 바꾼 뒤 다시 잰다.
- R의 캐릭터 프록시(2c10e34)가 이 장면에 적용되는지[실측, CPU]: 관 메시(60,096 삼각형)의 V LOD 절단은
  60096 / 2982 / 1274 / 932 / 466 / 232 / 116 / 58이고, `raytracing.character_proxy_triangles` 8000 안의 가장 고운 절단은 2,982 삼각형이다.
  확인: `unx_gate_host_hostdynamic --save-scene <f>`(내용만, 커밋 전 저장) → `unx_gate_host_hostscene --scene <f> --describe`("LOD cuts" 줄).
  R·S가 게임이 끝난 뒤 이 게이트로 refit·VSM 수정을 잰다(사용법을 두 세션에 보냈다).

## 2.3 실행 절차 (재현)

```text
1. build:   powershell -File Tools/CI/Build.ps1 -Track I -Tracks "V;M;S;R;C;I"         (UnravelNext.dll, 커널, 시험·게이트)
            build/I는 항상 이 트랙 집합으로 구성한다. 같은 폴더에서 -Tracks를 바꿔 재구성하면 CMake가 대상마다
            CXXDependInfo.json을 다시 쓰고, Ninja가 dyndep 단언(edge && !edge->outputs_ready())으로 멈춘다(2026-09-25 [실측]).
            그때는 build/I를 지우고 새로 빌드한다(111 s).
   측정용:  powershell -File Tools/CI/Build.ps1 -Track all -Committed                  (커밋 기준, ..\UnravelNext-gate\build\all:
            다른 트랙의 작업 중 파일에 영향받지 않는다. INTERFACES v1.12 3.5)
2. deploy:  powershell -File Native/Host/Deploy.ps1 [-Build ..\UnravelNext-gate\build\all] (Unity가 DLL을 싣고 있으면 거부;
            품질 파일은 그 빌드의 원본 트리에서 가져간다)
3. Unity:   감사 세션에 알리고, tasklist로 Unity.exe가 없는지 확인한 뒤
            Unity.exe -batchmode -quit -projectPath C:\Users\USER\Unravel -buildTarget Win64
                      -executeMethod UnravelNextBridge.DataWorld.Editor.UnravelNextDataWorldScene.BuildPlayer
            (원본 NativeDataWorld.unity는 두고 Assets/UnravelNextBridge/DataWorld/NativeDataWorld_UnravelNext.unity 사본 + Player)
4. 측정:   GpuLock.ps1 -Track I -- powershell -NoProfile -Command "Start-Process -Wait <Player>.exe
            '-screen-fullscreen 0 -screen-width 1280 -screen-height 720 -unxMeasureOut <json> -unxMeasureFrames 600'"
            (GUI exe라 Start-Process -Wait로 잠금을 실행 내내 잡는다)
            추가 인자: -unxExportScene <file.unxscene>(첫 동기화 뒤 현재 장면 저장), -unxScale 1024,256(설계 규모 부하 복제:
            실제 개체의 committed 루트·포즈를 읽는 인스턴스를 격자에 더한다)
   검사:   unx_gate_host_hostscene.exe --scene <file.unxscene> --describe      (CPU만: 태양·재질·메시별 NaN·감김·LOD 절단·RT 프록시·인스턴스)
            unx_gate_host_hostdynamic.exe --save-scene <file.unxscene>          (게이트 장면 내용 저장, 잠금 없음; 디바이스는 만든다)
5. 편집기: Unity.exe -projectPath ... -executeMethod UnravelNextBridge.DataWorld.Editor.UnravelNextEditorRun.Play -unxMeasureOut <json>
            (창 있는 편집기, Play 모드에서 같은 측정을 하고 스스로 종료)
```

## 2.4 남은 일 (I)

- 4K 규모 실행의 Unity 프레임 주기(13.4 ms) 원인을 프로파일러로 찾는다.
- 루트 동기화의 네이티브 경로(`nw_snapshot_read_components` + 네이티브 보간), 포즈는 `na_present_batch`가 생기면 연결한다.
- 바람 해제 패치(`UnxFrameSetEnvironment`의 바람 거부 제거와 시험 전환)는 준비됐고 S의 페이지 메타 완료 알림을 기다린다.
- 재질 v2(clearcoat·박막)가 코어에 들어오면 `UnxMaterialDesc` v2와 데이터 월드 변환.
- 규모: C# 어댑터는 인스턴스·관절마다 관리 코드로 보간·복사한다. RPP 부하(캐릭터 256, 강체 1,024)에서 메인 스레드 비용을 재고
  (`hostSynchronizeMs`), 렌더 제출 1 ms 할당을 넘으면 네이티브 어댑터로 옮긴다. NativeWorld의 `nw_snapshot_previous`와
  `nw_snapshot_read_components`, Animation 포즈 리스를 C++에서 직접 읽는 방식이다.
- 표시 경로가 없는 것: 소프트 바디(tick마다 변형되는 정점 → GpuScene의 동적 정점 API 필요), Matter·VFX(FX 트랙), 파괴 조각(적재 뒤
  인스턴스 추가 → 용량 있는 인스턴스 풀 필요), 개체에 붙은 광원(프레임별 광원 갱신 API 필요). 필요할 때 코어에 요청한다.

## 3. 요청

- `20260925_I_host_module.md`: A(등록)는 반영됐다(코어 3b0c049, v1.6). B(GpuScene 프레임 갱신)도 반영됐다(6af7de8, v1.8).
- `20260925_I_unity_queue_device.md`: Unity 디바이스·큐 위의 `Device`와 실행 훅이 반영됐다(2238b68, v1.9).
- `20260925_I_material_layers.md`(대기): clearcoat와 박막 간섭. 데이터 월드 재질 1(모든 동적 상자와 캐릭터)이 둘 다 쓴다.
  반영 전까지는 기저 층만 옮기고 로그에 남긴다. 측정 결과에도 이 차이를 함께 적는다.
  코어 설계 `Docs/Design/MATERIAL_LAYERS_KO.md`(740e80b)를 검토해 알렸다: 데이터 월드 재질 1의 값(clearcoat 1 / 0.12, 막 420 nm·1.4·덮임 0.35,
  기판 1.5 / 2)은 모두 담긴다. 이전 엔진은 기판 η·κ가 RGB라 float3를 제안했다(금속 기판의 색). 이전 엔진은 막 바깥을 늘 공기로 봤고 RGB 세 대역
  평균 Airy를 썼으므로 외형이 달라질 수 있다(정의는 설계대로). 설계 밖 이전 기능(유전체 경계 막, sheen, 이방성, 디테일 층)은 기록만 했다.
  I 몫: `UnxMaterialDesc` version 2(필드 추가, v1 152 B도 받음), `.unxscene` v2 적재·내보내기.
- `20260925_I_wind_change.md`(반영): 코어 v1.23(bf897d9), S의 페이지 규칙(그린 순간의 바람을 페이지 메타에 기록, `windChangeBound` 판정)이 들어간 뒤 `UnxFrameSetEnvironment`의 바람 거부를 풀었다. 이제 바람 속도 >= 0과 단위 방향만 검사한다. `unx_test_host_hostabi --live`[실측, 커밋 빌드 27edf60, GpuLock]: 바꾼 바람이 현재 장면 저장에 최신 값으로 들어가고 음수 속도는 거부된다.
  S 검토(bbf2575, `20260925_S_wind_change_review.md`): 구조에 동의한다. windOffset이 무기억 상태 함수라 페이지를 그린 순간과 지금의
  바람 끝점만 비교하면 되고(P3 모델도 무기억이어야 한다는 조건을 계약에 넣자는 제안), revision 없이 페이지 메타에 그 순간의 바람을 저장한다.
  상한은 I 초안보다 좁은 K·[s1²·0.4·min(2, 1.7|Δt|) + |s1² − s0²| + 2 sin(Δθ/2)·s0²]를 제안했다. R도 같은 의견이다(revision 불필요;
  바람 잎 정확 집합은 매 프레임 현재 바람으로 변형하고, refit을 건너뛸 때만 같은 끝점 상한을 쓴다). 합의: windRevision 없이 끝점 상한
  `windChangeBound`와 windOffset 무기억 계약. 코어가 INTERFACES를 올리면 커밋 뒤 바람 변경 거부를 푼다.
- `20260925_I_device_removed_policy.md`(반영, 코어 67b5508 v1.27): 플러그인이 `UnityPluginLoad`에서 `DeviceRemovedPolicy::Throw`를 둔다.
  ABI 5: `DeviceRemovedError`는 `UNX_DEVICE_REMOVED`(−4)가 되고, 그 뒤 모든 렌더러 호출이 같은 코드를 돌려준다(파괴는 된다). 렌더 이벤트는
  제거 뒤 아무것도 하지 않는다. 시험 `unx_test_host_hostdeviceremoved`(RemoveDevice로 이 프로세스의 장치만 제거; TDR 아님).
- 알려진 실패(VFX 세션, V3까지, 회귀 아님): NativeVfx V2가 설치되면 `NativeDataWorldPlayerValidation`의 "Data VFX did not execute on the shared
  GPU" 단언(EnableVfxGpu = true)이 실패한다. TitanNative 공유 GPU VFX 실행기가 퇴역했고, GPU 입자 모듈 연결은 V3(I가 FX 모듈을
  `VfxStreamExecutors.Provider` / `NV_StreamExecutor`로 잇는 일)다. EnableVfxGpu = false(`-unxVfxCpu`) 조건의 Player에는 영향이 없다.
  설치 뒤 TitanNative VFX 렌더러는 CPU 투영으로 그린다. `Docs/Rebuild/WORLD_VFX_DESIGN_KO.md` 9.4(a81466e5).
- `20260925_I_skin_normals.md`(대기): 스킨 법선을 관절 3×3의 여인수로 변환하는 것이다. 데이터 월드 캐릭터의 비균일 스케일(0.6, 0.8, 0.6)을 관절에 접으면 필요하다.

## 3.1 호스트 쪽 설계 조건 (날씨·시간대)

- **바람 모델은 무기억 상태 함수여야 한다**(P3 날씨 설계의 조건): 변위 windOffset은 (시각, 그 시각의 바람 상태: 방향·속도·돌풍 파라미터)만의
  함수여야 하고 과거 이력(적분된 위상, 관성 상태 등)에 기대면 안 된다. S의 페이지 판정과 R의 refit 생략이 "그린 순간의 바람"과 "지금의
  바람" 두 끝점만 비교하기 때문이다(`windChangeBound`). 돌풍·방향 회전처럼 이력이 있어 보이는 효과는 호스트가 바람 상태를 시각의 함수로
  내보내는 방식으로 만든다(예: 결정적 잡음을 시각으로 표본화). 합의 경위: `Requests/20260925_I_wind_change.md`,
  `20260925_S_wind_change_review.md`, R 의견(조율 세션 전달).
- 태양·대기는 프레임마다 바꿀 수 있다(`UnxFrameSetEnvironment`). 대기 파라미터 변경은 다중산란 LUT 재생성(S: 텍셀당 256스레드 구조)을
  부르므로 날씨 전환은 대기를 매 프레임이 아니라 필요한 만큼만 바꾸는 쪽이 싸다. 비용은 게임 뒤 잰다.

## 4. 이전 저장소 변경

- `Assets/UnravelNextBridge/` 새 폴더만. `Assets/TitanSRP`는 고치지 않았다. 프로브는 실행 중에 `GraphicsSettings.defaultRenderPipeline`을
  바꾸므로 ProjectSettings는 그대로다.
