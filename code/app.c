#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <errno.h>

// === smart_clock 드라이버 데이터 구조체 ===
typedef struct {
    int hours;
    int minutes;
    int seconds;
    int mode; // 0:Normal, 1:Set Hour, 2:Set Min
} clock_info_t;

// === (추가) DHT11 드라이버 데이터 구조체 (바이너리로 줄 때 대비) ===
typedef struct {
    int hum;   // 습도
    int temp;  // 온도
} dht11_info_t;

// === 폰트 데이터 ===
const unsigned char font5x7[][5] = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 0 (index 0)
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 1
    {0x42, 0x61, 0x51, 0x49, 0x46}, // 2
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // 4
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 6
    {0x01, 0x71, 0x09, 0x05, 0x03}, // 7
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // 9
    {0x08, 0x08, 0x08, 0x08, 0x08}, // - (index 10)
    {0x00, 0x36, 0x36, 0x00, 0x00}, // : (index 11)
    {0x00, 0x00, 0x00, 0x00, 0x00}, // space (index 12)
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // [ (index 13)
    {0x22, 0x41, 0x41, 0x41, 0x3E}, // ] (index 14)
    {0x00, 0x00, 0x00, 0x00, 0x00}  // safety
};

// 화면 버퍼
unsigned char buffer[1024];

// 폰트 인덱스 찾기
int get_font_index(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c == '-') return 10;
    if (c == ':') return 11;
    if (c == '[') return 13;
    if (c == ']') return 14;
    return 12; // 나머지는 공백
}

// 문자 하나 그리기
void draw_char_5x7(int page, int col, char c) {
    int i, font_idx = get_font_index(c);
    int start_index = (page * 128) + col;
    for (i = 0; i < 5; i++) {
        if (start_index + i < 1024 && col + i < 128) {
            buffer[start_index + i] = font5x7[font_idx][i];
        }
    }
}

// 문자열 그리기
void draw_string_5x7(int page, int start_col, char *str) {
    int i = 0;
    while (str[i] != '\0') {
        draw_char_5x7(page, start_col + (i * 6), str[i]);
        i++;
    }
}

// === 시스템 시간 동기화 함수 ===
void sync_system_time(int fd) {
    time_t rawtime;
    struct tm *ti;
    clock_info_t sys_time;

    time(&rawtime);
    ti = localtime(&rawtime);

    sys_time.hours = ti->tm_hour;
    sys_time.minutes = ti->tm_min;
    sys_time.seconds = ti->tm_sec;
    sys_time.mode = 0;

    write(fd, &sys_time, sizeof(clock_info_t));
    printf(">> System Time Synced to RTC: %02d:%02d:%02d\n",
           sys_time.hours, sys_time.minutes, sys_time.seconds);
}

/*
 * (추가) DHT11 읽기 함수
 * - 드라이버가 바이너리(struct)로 주는 경우 / 문자열로 주는 경우 둘 다 대응
 * - 성공하면 0 반환, 실패하면 -1 반환
 */
int read_dht11(int dht_fd, int *out_hum, int *out_temp) {
    if (dht_fd < 0) return -1;

    // 1) 바이너리 구조체로 오는 경우 시도
    dht11_info_t di;
    ssize_t n = read(dht_fd, &di, sizeof(di));
    if (n == (ssize_t)sizeof(di)) {
        *out_hum = di.hum;
        *out_temp = di.temp;
        return 0;
    }

    // 2) 문자열로 오는 경우 시도 (예: "HUM=60 TEMP=25\n" 또는 "60 25\n" 등)
    //    read가 구조체보다 적게 왔을 수도 있으니 다시 읽기(파일 오프셋/드라이버 구현에 따라 다름)
    //    단순화를 위해 lseek는 하지 않고 한 번 더 시도
    char s[64];
    n = read(dht_fd, s, sizeof(s) - 1);
    if (n <= 0) return -1;
    s[n] = '\0';

    int h = -1, t = -1;

    // 패턴1: HUM=xx TEMP=yy
    if (sscanf(s, "HUM=%d TEMP=%d", &h, &t) == 2) {
        *out_hum = h; *out_temp = t;
        return 0;
    }

    // 패턴2: "xx yy"
    if (sscanf(s, "%d %d", &h, &t) == 2) {
        *out_hum = h; *out_temp = t;
        return 0;
    }

    // 패턴3: "xx,yy"
    if (sscanf(s, "%d,%d", &h, &t) == 2) {
        *out_hum = h; *out_temp = t;
        return 0;
    }

    return -1;
}

int main() {
    int oled_fd, clock_fd, dht_fd;
    clock_info_t clk_info;

    // 날짜 및 표시용 변수
    time_t rawtime;
    struct tm *ti;
    char top_line_str[20];
    char time_str[20];

    // (추가) DHT 표시 문자열
    char dht_str[32];

    // 깜빡임 제어
    int blink_timer = 0;
    int show_text = 1;

    // (추가) DHT 읽기 주기 제어/캐시
    int dht_tick = 0;      // 0.1s 루프 기준 10번=1초
    int hum = -1, temp = -1;

    // 1. OLED 드라이버 열기
    oled_fd = open("/dev/my_oled", O_WRONLY);
    if (oled_fd == -1) { perror("OLED open fail"); exit(1); }

    // 2. 시계 드라이버 열기
    clock_fd = open("/dev/smart_clock", O_RDWR);
    if (clock_fd == -1) { perror("Clock open fail"); close(oled_fd); exit(1); }

    // 3. (추가) DHT11 드라이버 열기 (없어도 앱은 동작하도록 -1 처리)
    dht_fd = open("/dev/dht11_driver", O_RDONLY);
    if (dht_fd == -1) {
        perror("DHT11 open fail (continue without DHT)");
        dht_fd = -1;
    }

    // 4. 앱 시작 시 자동 시간 동기화
    sync_system_time(clock_fd);

    printf("UI Started with Auto-Sync + DHT...\n");

    while (1) {
        // smart_clock 상태 읽기
        if (read(clock_fd, &clk_info, sizeof(clock_info_t)) < 0) break;

        // 윗줄 날짜용 시스템 시간 읽기
        time(&rawtime);
        ti = localtime(&rawtime);

        // 깜빡임 타이머 (0.2초 주기)
        blink_timer++;
        if (blink_timer >= 2) {
            show_text = !show_text;
            blink_timer = 0;
        }

        // (추가) DHT11은 너무 자주 읽으면 실패율이 올라가서 1초에 1번만 읽고 캐시 사용
        dht_tick++;
        if (dht_tick >= 10) { // 0.1s * 10 = 1초
            dht_tick = 0;
            if (dht_fd != -1) {
                int nh, nt;
                if (read_dht11(dht_fd, &nh, &nt) == 0) {
                    hum = nh;
                    temp = nt;
                }
            }
        }

        // [윗줄] 날짜 고정 표시
        sprintf(top_line_str, "%04d-%02d-%02d", ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday);

        // [아랫줄] 시간 및 깜빡임 처리
        char hh[3], mm[3], ss[3];
        sprintf(hh, "%02d", clk_info.hours);
        sprintf(mm, "%02d", clk_info.minutes);
        sprintf(ss, "%02d", clk_info.seconds);

        if (clk_info.mode == 1 && show_text == 0) strcpy(hh, "  "); // 시 설정 모드 깜빡임
        if (clk_info.mode == 2 && show_text == 0) strcpy(mm, "  "); // 분 설정 모드 깜빡임

        sprintf(time_str, "%s:%s:%s", hh, mm, ss);

        // (추가) DHT 표시 문자열 만들기
        // 글자(H/T) 폰트가 없어서 bracket를 태그처럼 사용: [ ]HH [ ]TT
        // 습도/온도 값 없으면 "--"
        if (hum >= 0 && temp >= 0) {
            // 습도 0~99, 온도 -9~99 정도를 가정 (필요시 더 늘려도 됨)
            // 폰트에 '-'가 있으니 음수도 표현 가능
            char hbuf[4], tbuf[5];
            snprintf(hbuf, sizeof(hbuf), "%02d", hum);
            snprintf(tbuf, sizeof(tbuf), "%02d", temp);
            snprintf(dht_str, sizeof(dht_str), "[ ]%s [ ]%s", hbuf, tbuf);
        } else {
            snprintf(dht_str, sizeof(dht_str), "[ ]-- [ ]--");
        }

        // 화면 그리기
        memset(buffer, 0, 1024);

        draw_string_5x7(0, 10, top_line_str); // 날짜 (page 0)
        draw_string_5x7(2, 10, time_str);     // 시간 (page 2)
        draw_string_5x7(4, 10, dht_str);      // (추가) 온습도 (page 4)

        // OLED에 전송
        write(oled_fd, buffer, 1024);

        usleep(100000); // 0.1초 대기
    }

    if (dht_fd != -1) close(dht_fd);
    close(clock_fd);
    close(oled_fd);
    return 0;
}
