// rtc_control_driver.c
// DS1302 RTC 제어 + 로터리 엔코더로 시간 조정 + 앱에서 시간 write 가능

#include <linux/module.h>     // 커널 모듈 관련 (module_init, MODULE_LICENSE 등)
#include <linux/kernel.h>     // printk, pr_info, pr_err
#include <linux/init.h>       // __init, __exit
#include <linux/gpio.h>       // GPIO 요청, 설정, 값 읽기/쓰기
#include <linux/interrupt.h> // 인터럽트 등록/해제
#include <linux/delay.h>     // udelay (마이크로초 지연)
#include <linux/workqueue.h> // work_struct, schedule_work
#include <linux/timer.h>     // kernel timer
#include <linux/fs.h>        // register_chrdev, file_operations
#include <linux/uaccess.h>   // copy_to_user, copy_from_user

#define DEVICE_NAME "smart_clock" // /dev/smart_clock
#define DEVICE_MAJOR 230          // 문자 디바이스 메이저 번호

// ===== GPIO 핀 정의 =====
// DS1302 RTC 핀
#define GPIO_RTC_RST 16   // DS1302 RST (CE)
#define GPIO_RTC_CLK 20   // DS1302 SCLK
#define GPIO_RTC_DAT 21   // DS1302 I/O (Data)

// 로터리 엔코더 핀
#define GPIO_ROT_CLK 5    // A상 (CLK)
#define GPIO_ROT_DT  6    // B상 (DT)
#define GPIO_ROT_SW  13   // 버튼 (SW)

// 시간 정보 구조체 (유저앱과 그대로 주고받음)
typedef struct {
    int hours;    // 시
    int minutes;  // 분
    int seconds;  // 초
    int mode;     // 0: 정상 / 1: 시 설정 / 2: 분 설정
} clock_info_t;

// 현재 시계 상태 (커널 내부 상태)
static clock_info_t current_state;

// 인터럽트 번호 저장용
static int irq_rotary_clk;
static int irq_rotary_sw;

// 디바운싱용 마지막 인터럽트 시간
static unsigned long last_rotary_time = 0;
static unsigned long last_btn_time = 0;

// workqueue 구조체 (인터럽트에서 실제 처리 미루기)
static struct work_struct rotary_work;
static struct work_struct btn_work;

// 1초 주기 타이머
static struct timer_list my_timer;

/* =========================================================
 * DS1302 Low Level Bit-Banging
 * ========================================================= */

// DS1302로 1바이트 쓰기 (LSB first)
void ds1302_write_byte(unsigned char dat)
{
    int i;

    // DAT 핀을 출력으로 설정
    gpio_direction_output(GPIO_RTC_DAT, 0);

    // 8비트 전송
    for (i = 0; i < 8; i++) {
        // 현재 LSB를 DAT 핀으로 출력
        gpio_set_value(GPIO_RTC_DAT, dat & 0x01);
        udelay(2);

        // CLK 상승 에지
        gpio_set_value(GPIO_RTC_CLK, 1);
        udelay(2);

        // CLK 하강 에지
        gpio_set_value(GPIO_RTC_CLK, 0);
        udelay(2);

        // 다음 비트
        dat >>= 1;
    }
}

// DS1302에서 1바이트 읽기 (LSB first)
unsigned char ds1302_read_byte(void)
{
    int i;
    unsigned char dat = 0;

    // DAT 핀을 입력으로 설정
    gpio_direction_input(GPIO_RTC_DAT);

    for (i = 0; i < 8; i++) {
        dat >>= 1;

        // DAT 핀 값 읽기
        if (gpio_get_value(GPIO_RTC_DAT))
            dat |= 0x80;

        gpio_set_value(GPIO_RTC_CLK, 1);
        udelay(2);
        gpio_set_value(GPIO_RTC_CLK, 0);
        udelay(2);
    }
    return dat;
}

// DS1302 레지스터 쓰기
void ds1302_write_reg(unsigned char cmd, unsigned char data)
{
    gpio_set_value(GPIO_RTC_RST, 1);  // 통신 시작
    ds1302_write_byte(cmd);           // 주소 전송
    ds1302_write_byte(data);          // 데이터 전송
    gpio_set_value(GPIO_RTC_RST, 0);  // 통신 종료
    gpio_set_value(GPIO_RTC_CLK, 0);  // CLK 안정화
}

// DS1302 레지스터 읽기
unsigned char ds1302_read_reg(unsigned char cmd)
{
    unsigned char data;

    gpio_set_value(GPIO_RTC_RST, 1);  // 통신 시작
    ds1302_write_byte(cmd);           // 읽기 명령
    data = ds1302_read_byte();        // 데이터 수신
    gpio_set_value(GPIO_RTC_RST, 0);  // 통신 종료
    gpio_set_value(GPIO_RTC_CLK, 0);
    return data;
}

// BCD → 10진수 변환 매크로
#define BCD2BIN(val) (((val) & 0x0f) + ((val) >> 4) * 10)

// 10진수 → BCD 변환 매크로
#define BIN2BCD(val) ((((val) / 10) << 4) + ((val) % 10))

// RTC에서 현재 시간 읽기
void get_rtc_time(void)
{
    current_state.seconds = BCD2BIN(ds1302_read_reg(0x81));
    current_state.minutes = BCD2BIN(ds1302_read_reg(0x83));
    current_state.hours   = BCD2BIN(ds1302_read_reg(0x85));
}

// RTC에 현재 시간 쓰기
void set_rtc_time(void)
{
    ds1302_write_reg(0x8E, 0x00); // Write Protect OFF
    ds1302_write_reg(0x84, BIN2BCD(current_state.hours));
    ds1302_write_reg(0x82, BIN2BCD(current_state.minutes));
    ds1302_write_reg(0x80, BIN2BCD(current_state.seconds));
    ds1302_write_reg(0x8E, 0x80); // Write Protect ON
}

/* =========================================================
 * Logic Layer
 * ========================================================= */

// 1초마다 실행되는 타이머 콜백
void timer_callback(struct timer_list *t)
{
    // 정상 모드일 때만 내부 초 증가
    if (current_state.mode == 0) {
        current_state.seconds++;

        if (current_state.seconds > 59) {
            current_state.seconds = 0;
            current_state.minutes++;
            get_rtc_time(); // 분 단위로 RTC와 동기화
        }

        if (current_state.minutes > 59) {
            current_state.minutes = 0;
            current_state.hours++;
        }

        if (current_state.hours > 23)
            current_state.hours = 0;
    }

    // 다음 1초 타이머 재설정
    mod_timer(&my_timer, jiffies + msecs_to_jiffies(1000));
}

// 로터리 엔코더 회전 처리 (workqueue)
static void rotary_work_func(struct work_struct *work)
{
    int dt_val = gpio_get_value(GPIO_ROT_DT);

    // 회전 방향 판단
    int change = (dt_val != 1) ? 1 : -1;

    // 시 설정 모드
    if (current_state.mode == 1) {
        current_state.hours += change;
        if (current_state.hours > 23) current_state.hours = 0;
        else if (current_state.hours < 0) current_state.hours = 23;
        set_rtc_time();
    }
    // 분 설정 모드
    else if (current_state.mode == 2) {
        current_state.minutes += change;
        if (current_state.minutes > 59) current_state.minutes = 0;
        else if (current_state.minutes < 0) current_state.minutes = 59;
        current_state.seconds = 0;
        set_rtc_time();
    }
}

// 버튼 눌림 처리 (mode 변경)
static void btn_work_func(struct work_struct *work)
{
    current_state.mode++;
    if (current_state.mode > 2)
        current_state.mode = 0;
}

// 로터리 엔코더 인터럽트 핸들러
static irqreturn_t rotary_irq_handler(int irq, void *dev_id)
{
    unsigned long current_time = jiffies;

    // 디바운싱 (150ms)
    if (time_after(current_time, last_rotary_time + msecs_to_jiffies(150))) {
        last_rotary_time = current_time;
        schedule_work(&rotary_work);
    }
    return IRQ_HANDLED;
}

// 버튼 인터럽트 핸들러
static irqreturn_t button_irq_handler(int irq, void *dev_id)
{
    unsigned long current_time = jiffies;

    // 디바운싱 (200ms)
    if (time_after(current_time, last_btn_time + msecs_to_jiffies(200))) {
        last_btn_time = current_time;
        schedule_work(&btn_work);
    }
    return IRQ_HANDLED;
}

/* =========================================================
 * File Operations
 * ========================================================= */

// read(): 현재 시간 상태를 유저로 전달
static ssize_t clock_read(struct file *file,
                          char __user *buf,
                          size_t count,
                          loff_t *f_pos)
{
    if (copy_to_user(buf, &current_state, sizeof(clock_info_t)))
        return -EFAULT;

    return sizeof(clock_info_t);
}

// write(): 앱에서 전달된 시간으로 RTC 설정
static ssize_t clock_write(struct file *file,
                           const char __user *buf,
                           size_t count,
                           loff_t *f_pos)
{
    clock_info_t new_time;

    if (copy_from_user(&new_time, buf, sizeof(clock_info_t)))
        return -EFAULT;

    current_state.hours   = new_time.hours;
    current_state.minutes = new_time.minutes;
    current_state.seconds = new_time.seconds;

    set_rtc_time(); // 하드웨어 RTC에 반영
    return count;
}

// 파일 오퍼레이션 구조체
static struct file_operations clock_fops = {
    .owner = THIS_MODULE,
    .read  = clock_read,
    .write = clock_write,
};

/* =========================================================
 * Module Init / Exit
 * ========================================================= */

static int __init my_driver_init(void)
{
    // 문자 디바이스 등록
    register_chrdev(DEVICE_MAJOR, DEVICE_NAME, &clock_fops);

    // RTC GPIO 설정
    gpio_request(GPIO_RTC_RST, "RTC_RST");
    gpio_direction_output(GPIO_RTC_RST, 0);

    gpio_request(GPIO_RTC_CLK, "RTC_CLK");
    gpio_direction_output(GPIO_RTC_CLK, 0);

    gpio_request(GPIO_RTC_DAT, "RTC_DAT");

    // 로터리 엔코더 GPIO
    gpio_request(GPIO_ROT_CLK, "ROT_CLK");
    gpio_direction_input(GPIO_ROT_CLK);

    gpio_request(GPIO_ROT_DT, "ROT_DT");
    gpio_direction_input(GPIO_ROT_DT);

    gpio_request(GPIO_ROT_SW, "ROT_SW");
    gpio_direction_input(GPIO_ROT_SW);

    // 초기 시간 로드
    get_rtc_time();

    // workqueue 초기화
    INIT_WORK(&rotary_work, rotary_work_func);
    INIT_WORK(&btn_work, btn_work_func);

    // 타이머 설정
    timer_setup(&my_timer, timer_callback, 0);
    mod_timer(&my_timer, jiffies + msecs_to_jiffies(1000));

    // 인터럽트 등록
    irq_rotary_clk = gpio_to_irq(GPIO_ROT_CLK);
    request_irq(irq_rotary_clk, rotary_irq_handler,
                IRQF_TRIGGER_FALLING, "rot_clk", NULL);

    irq_rotary_sw = gpio_to_irq(GPIO_ROT_SW);
    request_irq(irq_rotary_sw, button_irq_handler,
                IRQF_TRIGGER_FALLING, "rot_sw", NULL);

    return 0;
}

static void __exit my_driver_exit(void)
{
    unregister_chrdev(DEVICE_MAJOR, DEVICE_NAME);

    del_timer(&my_timer);

    free_irq(irq_rotary_clk, NULL);
    free_irq(irq_rotary_sw, NULL);

    gpio_free(GPIO_RTC_RST);
    gpio_free(GPIO_RTC_CLK);
    gpio_free(GPIO_RTC_DAT);
    gpio_free(GPIO_ROT_CLK);
    gpio_free(GPIO_ROT_DT);
    gpio_free(GPIO_ROT_SW);
}

module_init(my_driver_init);
module_exit(my_driver_exit);

MODULE_LICENSE("GPL");
