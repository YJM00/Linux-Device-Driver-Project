# ⏰ OLED Smart Clock (Linux Device Driver Project)
> **Linux 커널 디바이스 드라이버 + 유저 앱(User App)** 으로 구현한 스마트 시계 프로젝트  
> **OLED(SSD1306)** 에 **RTC(DS1302) 날짜/시간** + **DHT11 온습도**를 출력하고,  
> **로터리 엔코더(GPIO 인터럽트)** 로 **연/월/일/시/분(등)** 을 설정합니다.


- **플랫폼:** Raspberry Pi (Linux)  
- **핵심 키워드:** Linux Kernel Module, Character Device Driver, GPIO Interrupt, I2C(SSD1306), Bit-banging(DS1302/DHT11), User ↔ Kernel ↔ Hardware

---

## 1) 프로젝트 소개
이 프로젝트는 리눅스에서 하드웨어를 **파일(`/dev/...`)처럼 접근**할 수 있도록  
**커널 디바이스 드라이버(.ko)** 를 구현하고, 유저 영역의 **app.c**가 드라이버를 통해 데이터를 읽어 OLED에 표시하는 구조입니다.

### ✅ 구현 기능
- **RTC(DS1302)** 날짜/시간 읽기 및 설정
- **로터리 엔코더**로 날짜/시간 값 변경 (GPIO 인터럽트 기반)
- **DHT11** 온습도 측정 및 표시
- **OLED(SSD1306, I2C)** 에 날짜/시간 + 온습도 출력
- 드라이버(커널)와 앱(유저)로 분리하여 모듈 구조화

---

## 2) 전체 시스템 구조 (User ↔ Kernel ↔ Hardware)

### 📌 데이터 흐름
[User App (app.c)]
├─ RTC 값 읽기: read(/dev/smart_clock) -> 날짜/시간 수신
├─ DHT11 읽기: read(/dev/dht11_driver) -> 온습도 수신
├─ OLED 화면 구성: 128x64 프레임버퍼(1024B) 생성
└─ OLED 출력: write(/dev/my_oled) -> 프레임버퍼 전송

[Kernel Drivers (.ko)]
├─ rtc_control_driver.ko -> /dev/smart_clock (DS1302 + 로터리 입력 처리)
├─ oled_driver.ko -> /dev/my_oled (SSD1306 I2C 출력)
└─ dht11_driver.ko -> /dev/dht11_driver (온습도 측정)


### 📌 드라이버별 역할
- **rtc_control_driver**
  - DS1302를 GPIO bit-bang 방식으로 제어하여 날짜/시간을 읽음
  - 로터리 엔코더를 GPIO 인터럽트로 받아 설정 모드를 변경하고 값 증가/감소 처리
  - `/dev/smart_clock`를 통해 유저 앱에 날짜/시간 제공 및 설정 반영

- **oled_driver**
  - SSD1306 OLED를 I2C로 초기화하고 프레임버퍼(1024B)를 전송
  - `/dev/my_oled`에 write 된 데이터를 그대로 OLED에 출력

- **dht11_driver**
  - DHT11 센서의 타이밍 프로토콜(핸드셰이크 + bit stream)을 구현해 값 수신
  - 체크섬 검증 후 온도/습도 값을 `/dev/dht11_driver`로 제공

---

## 3) 하드웨어 구성

### 🧩 사용 부품
- Raspberry Pi (Linux)
- **OLED SSD1306 (I2C, 128x64, 주소 0x3C)**
- **RTC DS1302**
- **Rotary Encoder (CLK/DT/SW)**
- **DHT11 (DATA 단일 핀)**

### 🔌 회로도
<img width="1437" height="849" alt="스크린샷 2025-12-27 135003" src="https://github.com/user-attachments/assets/14a9d071-a7fd-402f-89d4-2aacb36734bc" />



---

## 4) 프로젝트 파일 구성
├── app.c
├── rtc_control_driver.c
├── oled_driver.c
├── dht11_driver.c
└── Makefile

- **app.c**
  - RTC/DHT11 값을 `/dev/*`에서 읽어온 뒤
  - OLED 화면을 프레임버퍼로 구성해서 `/dev/my_oled`로 write

- **rtc_control_driver.c**
  - DS1302 시간 read
  - 로터리 엔코더 인터럽트로 시간/날짜 설정 로직 수행
  - `/dev/smart_clock` 제공

- **oled_driver.c**
  - SSD1306 초기화 + 프레임버퍼 출력
  - `/dev/my_oled` 제공

- **dht11_driver.c**
  - DHT11 handshake + 타이밍 측정으로 값 수신/검증
  - `/dev/dht11_driver` 제공

---
## 동작 영상

https://github.com/user-attachments/assets/00de1c57-ba2d-42da-bdf1-1bc185c3b57f




---
## 5) 빌드 & 실행 방법

### 5-1. 커널 헤더 설치
```bash
sudo apt update
sudo apt install -y raspberrypi-kernel-headers build-essential
```


### 5-2. Makefile
code 파일에 있는 Makefile 이용

### 5-3. 빌드
```bash
make
```



### 5-4. 드라이버 로드
```bash
sudo insmod rtc_control_driver.ko
sudo insmod oled_driver.ko
sudo insmod dht11_driver.ko
```

로드 확인:
```bash
lsmod | grep -E "rtc_control_driver|oled_driver|dht11"
dmesg | tail -n 50
```



### 5-5. 디바이스 파일 확인
```bash
ls -l /dev/smart_clock /dev/my_oled /dev/dht11_driver
```

#### (선택) /dev 노드가 없을 때
- 드라이버 구현 방식에 따라 `/dev`가 자동 생성되지 않을 수 있습니다.
- major/minor는 **네 코드(dmesg 출력 또는 소스)** 기준으로 맞춰야 합니다.

예시(major/minor는 네 코드 기준으로 수정):
```bash
sudo mknod /dev/smart_clock c 230 0
sudo mknod /dev/my_oled c 231 0
sudo chmod 666 /dev/smart_clock /dev/my_oled
```

### 5-6. 앱 실행
```bash
gcc -o app app.c
./app
```

---

## 6) 동작 방식(요약)

### FSM
<img width="739" height="366" alt="image" src="https://github.com/user-attachments/assets/2213402e-39d3-4ff6-abc1-62949e1f09dd" />


### RTC 날짜/시간 표시
- 커널 드라이버가 DS1302에서 날짜/시간을 읽어 내부 상태 갱신
- 유저 앱이 `/dev/smart_clock`에서 `read()`로 값 수신 → OLED 표시

### 로터리 엔코더로 날짜/시간 설정
- 로터리 엔코더는 GPIO 인터럽트로 입력을 처리
- 설정 모드(예: Year → Month → Day → Hour → Min)를 전환하고
- 회전(CW/CCW)으로 값 증가/감소
- 변경된 값은 RTC에 반영되어 OLED에 즉시 갱신

### DHT11 온습도 표시
- DHT11은 타이밍 기반이라 너무 자주 읽으면 실패율이 증가
- 보통 **1초 이상 주기**로 읽어서 OLED에 갱신하는 방식이 안정적

### OLED 출력
- 유저 앱이 128x64 화면을 **1024바이트 프레임버퍼**로 구성
- `/dev/my_oled`에 `write()`하면 드라이버가 SSD1306로 I2C 전송하여 출력

---

## 7) 트러블슈팅 및 배운점

### 1) 로터리 엔코더 처리에 Workqueue를 사용한 이유
- 로터리 엔코더 입력을 **GPIO 인터럽트 방식**으로 처리했다.
- 인터럽트는 CPU가 하던 일을 멈추고 **ISR(인터럽트 핸들러)로 점프**해 실행되는 구조라서,  
  핸들러 내부에서 오래 걸리는 작업을 수행하면 시스템 지연/응답성이 나빠질 수 있다.
- 그래서 ISR에서는 **디바운싱(간단한 조건 체크)** 정도만 하고,  
  실제로 RTC 값을 증가/감소시키는 핵심 로직(시간 계산 + DS1302 쓰기)은  
  **workqueue에 등록(schecule_work)** 하여 **나중에 커널 워커 스레드에서 처리**하도록 설계했다.


### 2) DS1302(RTC 모듈) 제어에 비트뱅잉(Bit-banging)을 사용한 이유
- DS1302는 클럭 핀이 있어 **동기식 통신**처럼 보였고, 처음에는 I2C/SPI로 제어하려고 했다.
- 하지만 DS1302는 표준 I2C/SPI처럼 바로 붙일 수 있는 구조가 아니고(특히 I2C는 아님),  
  보드/설계 상황에서 **전용 컨트롤러를 그대로 쓰기 애매**했다.
- 결국 GPIO로 직접 클럭과 데이터 타이밍을 만들어서 통신하는 **비트뱅잉 방식**으로 구현했다.
- 결론적으로 DS1302는 “I2C 장치”가 아니라, **GPIO 토글 기반 제어(비트뱅잉)**가 구현 난이도 대비 가장 확실했다.


### 3) I2C 슬레이브 주소 개념 착각
- I2C 통신에서 “주소”는 처음에 MCU(마스터) 쪽 데이터시트에 의해 정해진다고 착각했다.
- 실제로는 **슬레이브 장치(예: SSD1306 OLED)**가 가지는 주소가 있고,  
  그 값은 **장치 데이터시트에 고정**되어 있거나, 모듈 점퍼/핀 설정에 따라 바뀐다(예: 0x3C / 0x3D).
- 이후 “마스터는 버스 제어(클럭/START/STOP)”, “슬레이브는 주소로 선택됨” 구조를 명확히 이해하게 됐다.


### 4) 리눅스 디바이스 드라이버에서 I2C/SPI 통신 방식 이해
- DS1302 제어처럼 GPIO로 직접 통신하는 방식과 달리,
  리눅스에서는 I2C/SPI가 **커널 서브시스템(버스 드라이버)**로 이미 존재한다.
- 즉, 보통은 “통신을 위해 내가 직접 /dev 파일을 새로 만드는 방식”이 아니라,
  - **I2C adapter(버스/컨트롤러)**를 통해
  - 해당 주소의 **i2c_client(슬레이브 디바이스)**로 통신한다.
- 정리하면, 리눅스에서 I2C/SPI는 “버스 프레임워크를 타고 들어가서” 통신하는 구조이며,
  드라이버는 `i2c_master_send()` 같은 커널 API를 통해 전송하게 된다.


---











