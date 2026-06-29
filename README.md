# MCTP-over-I2C Validator (Aardvark)

Total Phase **Aardvark** I2C/SPI 어댑터를 사용해, I2C(SMBus) 버스에 연결된 실제 MCTP
엔드포인트(DUT)와의 **MCTP 통신을 검증**하는 도구입니다.
[openbmc/libmctp](https://github.com/openbmc/libmctp)를 프로토콜 스택으로 사용하고,
Aardvark를 물리 I2C 계층으로 연결합니다.

OpenBMC의 MCTP 엔드포인트처럼 **master(requester) + slave(responder) 두 역할을 동시에**
수행하며, 각 역할을 validator 형식(PASS/FAIL)으로 점검합니다.

## 동작 개요

libmctp의 i2c 바인딩([`libmctp/i2c.c`](libmctp/i2c.c))은 물리 전송을 직접 하지 않고
**콜백 방식**입니다. 이 도구가 그 콜백 자리에 Aardvark를 끼워 넣는 글루(glue) 역할을 합니다.

```
  ┌─────────────┐   tx 콜백 (aa_i2c_write)    ┌──────────────┐
  │   libmctp   │ ─────────────────────────►  │              │
  │  (MCTP 스택) │                              │   Aardvark   │ ──► I2C 버스 ──► DUT
  │             │ ◄─────────────────────────  │  (USB↔I2C)   │
  └─────────────┘   mctp_i2c_rx (slave read)  └──────────────┘
```

- **송신(master)**: libmctp `tx_fn` → `aa_i2c_write` (선두 dest 주소 바이트 제거 후 전송)
- **수신(slave)**: `aa_async_poll` → `aa_i2c_slave_read` → (PEC 제거) → `mctp_i2c_rx`

> **중요**: MCTP-over-SMBus에서 응답은 *별도의 SMBus 트랜잭션*으로 옵니다. DUT가 master가
> 되어 우리 slave 주소로 응답을 write하므로, 요청의 응답을 받기 위해서도 slave가 필수입니다.

## 디렉터리 구성

| 경로 | 설명 |
|------|------|
| [`mctp_aardvark_test.c`](mctp_aardvark_test.c) | validator 소스 (master/slave/discovery/interactive) |
| [`build_mctp_test.sh`](build_mctp_test.sh) | 빌드 스크립트 |
| [`run_mctp_test.sh`](run_mctp_test.sh) | 실행 래퍼 (`aardvark.so`를 dlopen 경로에 등록) |
| [`libmctp_build.sh`](libmctp_build.sh) | libmctp 서브모듈 빌드 |
| `libmctp/` | openbmc/libmctp 서브모듈 |
| `aardvark-api-linux-x86_64-v6.00/` | Total Phase Aardvark SDK (C API + `aardvark.so`) |

## 사전 준비

### 1. libmctp 빌드

```bash
git submodule update --init     # 처음 클론한 경우
./libmctp_build.sh              # meson/ninja로 libmctp.so 생성
```

### 2. USB 권한 (Aardvark)

Aardvark는 FTDI 기반 USB 장치라 기본적으로 root만 접근 가능합니다. 일반 사용자가 열려면
권한을 부여해야 합니다(권한 없으면 `aa_open`이 `-7 UNABLE_TO_OPEN`, 감지 시 `(in-use)`).

**즉시(임시):**
```bash
sudo chmod 666 /dev/bus/usb/<BUS>/<DEV>   # lsusb -d 0403:e0d0 로 번호 확인
```

**영구(udev 규칙, 권장):** `/etc/udev/rules.d/99-aardvark.rules` 생성:
```
SUBSYSTEM=="usb", ATTR{idVendor}=="0403", ATTR{idProduct}=="e0d0", MODE="0666", GROUP="plugdev"
```
```bash
sudo udevadm control --reload && sudo udevadm trigger   # 후 장치 재연결
```

> WSL2에서는 usbipd로 장치를 attach해야 하며, udev가 비활성일 수 있어 `chmod` 방식이
> 더 확실할 수 있습니다.

## 빌드

```bash
./build_mctp_test.sh
```

## 실행

```bash
./run_mctp_test.sh [옵션]
```

`run_mctp_test.sh`는 `LD_LIBRARY_PATH`를 잡아주므로 어느 디렉터리에서든 동작합니다.

### 주요 사용 예

```bash
./run_mctp_test.sh -C            # master + slave validator (PEC 사용)
./run_mctp_test.sh -C -m         # MASTER 테스트만 (실제 DUT 왕복)
./run_mctp_test.sh -S            # SLAVE 자가검증만 (DUT 불필요)
./run_mctp_test.sh -A -C         # 자동 디스커버리 (지정 peer)
./run_mctp_test.sh -R -C         # 전체 I2C 버스 스캔
./run_mctp_test.sh -C -i         # 대화형 셸 (g / ver / types / q)
./run_mctp_test.sh -C -v         # 와이어 덤프 + libmctp 디버그 로그
```

종료 코드는 실패가 있으면 1이라 CI에 연동할 수 있습니다.

> **타겟 자동 탐지**: `-d`/`-E`를 주지 않으면 버스를 스캔해 첫 엔드포인트를 찾아 그 주소·EID로
> 테스트합니다(`-d`만 주면 해당 주소의 EID를 자동 학습). 즉 주소를 몰라도 동작합니다.

## 테스트 모드

| 모드 | 검증 내용 | 하드웨어 왕복 |
|------|-----------|---------------|
| **MASTER** (`-m`, 기본) | DUT에 Get EID / Version / Message Type Support 요청 후 응답(cc·필드) 검증 | ✅ 실제 버스 |
| **SLAVE** (`-S`) | 가짜 요청을 `mctp_i2c_rx`에 주입 → libmctp 자동 응답을 캡처해 내용 검증 | ❌ 소프트웨어 |
| **DISCOVERY** (`-A`/`-R`) | NULL-EID 물리주소로 Get EID → 능력 조회 (OpenBMC mctpd식 열거) | ✅ 실제 버스 |
| **INTERACTIVE** (`-i`) | 수동으로 컨트롤 명령 송신 + 들어오는 요청 자동 응답 | ✅ 실제 버스 |
| **LIVE** (`-L secs`) | DUT가 우리에게 보내는 요청을 일정 시간 관찰 | ✅ 실제 버스 |

> SLAVE 자가검증은 responder *로직*(프레임 파싱 → 응답 생성 → 프레이밍)을 결정론적으로
> 검증합니다. 외부 master ↔ Aardvark slave 간 **하드웨어 왕복**까지 보려면 두 번째 Aardvark를
> master로 쓰거나, DUT가 우리에게 요청을 보내는 경우 `-L`로 관찰하세요.

## 명령행 옵션

| 옵션 | 의미 | 기본값 |
|------|------|--------|
| `-p port` | Aardvark 포트 | 0 |
| `-b kHz` | I2C 비트레이트 | 100 |
| `-s addr` | 자기 7-bit I2C 주소 | 0x20 |
| `-e eid` | 자기 MCTP EID | 8 |
| `-d addr` | peer 7-bit I2C 주소 | 자동 디스커버리 |
| `-E eid` | peer MCTP EID | 자동 학습 |
| `-t ms` | master 응답 타임아웃 | 1000 |
| `-C` | SMBus PEC(CRC-8) 사용 | off |
| `-u` | I2C 풀업 활성화 | off |
| `-P` | 타겟 전원(5V) 공급 | off |
| `-m` / `-S` | MASTER만 / SLAVE만 | - |
| `-A` / `-R` | 디스커버리 / 버스 스캔 | - |
| `-x eid` | 디스커버리 중 EID 할당(Set Endpoint ID) | - |
| `-L secs` | LIVE 리스닝 | - |
| `-i` | 대화형 셸 | - |
| `-v` | 상세 출력 | off |

## SMBus PEC 주의

[`libmctp/i2c.c`](libmctp/i2c.c)는 PEC를 붙이지 않습니다(Linux 커널 SMBus 계층이 처리하는 것을
전제). Aardvark는 raw I2C이므로, **PEC를 요구하는 DUT**와 통신하려면 `-C`로 송신 시 PEC 추가 /
수신 시 검증·제거를 켜야 합니다. `-C` 없이 응답이 사라지면(libmctp가 `bytecount != len-3`으로
조용히 폐기) PEC를 의심하세요.

## 라이선스

`mctp_aardvark_test.c`는 Apache-2.0. `libmctp`, Aardvark SDK는 각 디렉터리의 라이선스를 따릅니다.
