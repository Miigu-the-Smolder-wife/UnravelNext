# 요청: 장치 제거 처리 방식을 프로세스별로 정하게 (I 트랙, 2026-09-25)

## 왜 필요한가

v1.26(feb9595)에서 `check()`와 `Queue::waitCpu`가 장치 제거(DEVICE_REMOVED/HUNG/RESET, 제거된 장치의 UINT64_MAX 펜스)를 만나면
`deviceRemoved()`가 "UNX_DEVICE_REMOVED ..."를 찍고 `_Exit(87)`로 프로세스를 끝낸다. 도구·시험·게이트에는 맞다(GpuLock이 87을 기록한다).

호스트 DLL(`UnravelNext.dll`)은 Unity 프로세스 안에서 돈다. 렌더러의 기록·제출은 Unity 렌더 스레드(플러그인 이벤트) 위에서, 적재는 메인
스레드에서 일어난다. 거기서 `_Exit(87)`이 불리면:
- Unity 편집기나 Player가 저장·정리 없이 즉시 끝난다(편집기면 사용자의 작업 장면도 잃는다).
- Unity 자신의 장치 손실 처리(장치 재생성 시도, `kUnityGfxDeviceEventShutdown/Initialize`)와 크래시 보고(덤프, 로그)가 돌지 않는다.
  13:56 데이터 월드 TDR을 풀 때 쓴 것이 바로 Unity의 덤프와 로그였다.
- 장치는 Unity 것이다. 장치가 제거됐을 때 무엇을 할지는 호스트가 정해야 한다.

## 원하는 변경

1. 프로세스별 처리 방식: `unx::render::setDeviceRemovedPolicy(DeviceRemovedPolicy)`(또는 처리 함수 등록).
   - `Exit`(기본): 지금과 같다(마지막 줄 UNX_DEVICE_REMOVED, `_Exit(87)`).
   - `Throw`: 같은 줄을 로그로 남기고 `unx::render::DeviceRemovedError`(std::runtime_error 파생, `what`, `hr`, `reason`을 담는다)를 던진다.
   호스트 DLL은 `UnityPluginLoad`에서 `Throw`로 둔다. 독립 실행 호스트 도구(내 게이트·시험)는 기본값 `Exit`를 그대로 쓴다.
2. `Throw` 방식에서 소멸자·noexcept 경로: 던지면 `std::terminate`가 된다. `Device::waitIdle`이 소멸자에서 불릴 때(예: `FrameRenderer`,
   `RenderGraph`, `GpuScene` 해제 중) 장치가 이미 제거됐으면 기다리지 말고 돌아오면 된다(제거된 장치에는 기다릴 GPU 작업이 없다).
   코어 쪽 소멸자에서 부르는 경로가 이 규칙을 지키도록 해 달라. I의 `~HostRenderer`는 I가 직접 잡는다.
3. `deviceRemoved`의 `[[noreturn]]`은 그대로 둘 수 있다(던지기도 돌아오지 않는다).

## I 쪽 처리(요청이 반영되면)

- ABI 결과 코드 `UNX_DEVICE_REMOVED = -4`를 더한다. `call()` 래퍼가 `DeviceRemovedError`를 이 코드로 돌려주고 `UnxLastError`에 what·hr·reason을 남긴다.
  렌더러는 그 뒤 모든 호출에 같은 코드를 돌려준다(쓸 수 없는 상태).
- C#: 이 코드를 받으면 렌더를 멈추고 렌더러를 버린다. Unity가 장치를 다시 만들면(Shutdown → Initialize 이벤트) 장면을 다시 적재한다.
- 렌더 이벤트(Unity 렌더 스레드)에서 난 제거는 다음 메인 스레드 호출이 코드로 받는다.

## 영향

- 코어: `Device.cpp`의 `deviceRemoved`와 처리 방식 저장, 소멸자 경로의 기다림. 도구·게이트의 동작은 바뀌지 않는다(기본값 Exit).
- I: 위 처리.

## 결과 (코어, 2026-09-25, INTERFACES v1.27)

- `setDeviceRemovedPolicy(DeviceRemovedPolicy::Exit | Throw)`, `deviceRemovedPolicy()`, `deviceWasRemoved()`, `struct DeviceRemovedError : Error { where, hr, reason }`(`D3D12.h`)를 넣었다.
- Throw: `check()` 경로는 `DeviceRemovedError`를 던진다. `Queue::signal/waitCpu`, `Device::waitIdle`은 제거를 기록하고 던지지 않고 돌아온다. `UNX_DEVICE_REMOVED ...` 줄은 로그에 한 번 남는다.
- 호스트 장치(`externalDevice`) 위의 첫 장치는 명시 설정이 없으면 Throw다. 호스트는 `UnityPluginLoad`에서 명시해도 된다.
- [실측] 단위 테스트 `device_removed_exit_policy`(자식 종료 코드 87과 마지막 줄), `zz_device_removed_throw_policy`(예외 필드, 이어진 waitIdle은 던지지 않음). 30/30, debug layer 0.
