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

## 7) 트러블슈팅

### OLED가 안 뜸
- I2C 활성화:
  - `sudo raspi-config` → Interface Options → I2C Enable
- I2C 주소 확인:
```bash
i2cdetect -y 1
```
- SSD1306 주소(보통 `0x3C`)가 보이는지 확인
- SDA/SCL/VCC/GND 배선 점검

### DHT11 값이 자주 실패/0으로 뜸
- 1초 이상 간격 권장
- DATA 라인 풀업 상태 확인
- 배선 길이/접촉 불량 점검

### 로터리 엔코더 튐(오동작)
- 디바운싱 시간 조절 필요
- CLK/DT/SW 풀업/풀다운 확인

---

## 8) 배운 점
- Character Device Driver 구현을 통해 `/dev` 기반 HW 제어 흐름 이해
- GPIO 인터럽트 기반 이벤트 처리(디바운싱/상태처리) 경험
- RTC/센서/디스플레이를 모듈로 분리하여 확장 가능한 구조 설계 경험

---

## 9) 향후 개선 사항
- 날짜 설정 범위 확장(윤년/월별 날짜 처리)
- OLED UI 개선(큰 폰트, 아이콘, 레이아웃)
- udev rule로 디바이스 노드/권한 자동화
- systemd 서비스로 부팅 시 자동 실행




