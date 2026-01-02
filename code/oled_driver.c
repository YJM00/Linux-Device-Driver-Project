#include <linux/module.h>     // 커널 모듈 관련 매크로 (module_init, MODULE_LICENSE 등)
#include <linux/kernel.h>     // printk, pr_info, pr_err 같은 커널 로그
#include <linux/init.h>       // __init, __exit 매크로
#include <linux/fs.h>         // file_operations, register_chrdev
#include <linux/uaccess.h>    // copy_from_user (유저 ↔ 커널 메모리 복사)
#include <linux/i2c.h>        // I2C 서브시스템 (i2c_adapter, i2c_client)
#include <linux/cdev.h>       // 문자 디바이스 구조체 (이번 코드는 register_chrdev만 사용)
#include <linux/slab.h>       // kmalloc, kfree (커널 동적 메모리 할당)

#define DRIVER_NAME "my_oled" // /dev/my_oled 디바이스 이름
#define DRIVER_MAJOR 231      // 문자 디바이스 메이저 번호 (고정 사용)

// SSD1306 OLED I2C 주소 (보통 0x3C 또는 0x3D)
#define OLED_I2C_ADDR 0x3C

// 라즈베리파이 기본 I2C 버스 번호 (/dev/i2c-1)
#define I2C_BUS_NUM   1

// I2C 버스를 나타내는 구조체 포인터
static struct i2c_adapter *oled_i2c_adapter = NULL;

// I2C 슬레이브(SSD1306 OLED)를 나타내는 구조체 포인터
static struct i2c_client *oled_i2c_client = NULL;

/*
 * SSD1306 초기화 명령어 테이블
 * open() 시 한 바이트씩 I2C Command로 전송됨
 */
static const unsigned char oled_init_cmds[] = {
    0xAE,       // Display OFF
    0x00,       // Set lower column start address
    0x10,       // Set higher column start address
    0x40,       // Set display start line
    0x81, 0xCF, // Set contrast
    0xA1,       // Segment remap (좌우 반전)
    0xC8,       // COM scan direction (상하 반전)
    0xA6,       // Normal display (반전 아님)
    0xA8, 0x3F, // Multiplex ratio (1/64)
    0xD3, 0x00, // Display offset
    0xD5, 0x80, // Display clock divide ratio
    0xD9, 0xF1, // Pre-charge period
    0xDA, 0x12, // COM pins hardware configuration
    0xDB, 0x40, // VCOMH deselect level
    0x20, 0x00, // Memory addressing mode = Horizontal
    0x8D, 0x14, // Charge pump enable
    0xAF        // Display ON
};

/*
 * SSD1306에 "명령(Command)" 1바이트를 I2C로 전송
 */
static int oled_i2c_write_cmd(unsigned char cmd)
{
    // SSD1306 I2C 프로토콜:
    // 첫 바이트 0x00 → Command
    unsigned char buf[2] = {0x00, cmd};

    // I2C로 2바이트 전송
    if (i2c_master_send(oled_i2c_client, buf, 2) != 2) {
        pr_err("OLED: Failed to send command 0x%02X\n", cmd);
        return -EIO; // I/O 에러
    }
    return 0;
}

/*
 * SSD1306에 "데이터(Data)" 블록을 I2C로 전송
 * 화면 픽셀 데이터용
 */
static int oled_i2c_write_data(unsigned char *data, int len)
{
    unsigned char *buf;
    int ret;

    // 데이터 앞에 컨트롤 바이트(0x40)를 붙이기 위해 len+1 할당
    buf = kmalloc(len + 1, GFP_KERNEL);
    if (!buf)
        return -ENOMEM;

    // 0x40 → Data 모드
    buf[0] = 0x40;

    // 유저 데이터 복사
    memcpy(buf + 1, data, len);

    // I2C로 한 번에 전송
    if (i2c_master_send(oled_i2c_client, buf, len + 1) != len + 1) {
        pr_err("OLED: Failed to send data\n");
        ret = -EIO;
    } else {
        ret = len;
    }

    kfree(buf);
    return ret;
}

/*
 * /dev/my_oled open() 호출 시 실행
 * → OLED 초기화 수행
 */
static int oled_open(struct inode *inode, struct file *file)
{
    int i;

    pr_info("OLED: Device opened, initializing OLED\n");

    // SSD1306 초기화 명령어 순차 전송
    for (i = 0; i < sizeof(oled_init_cmds); i++)
        oled_i2c_write_cmd(oled_init_cmds[i]);

    return 0;
}

/*
 * /dev/my_oled close() 호출 시 실행
 */
static int oled_release(struct inode *inode, struct file *file)
{
    return 0;
}

/*
 * /dev/my_oled write() 호출 시 실행
 * → 최대 1024바이트를 OLED 화면에 출력
 */
static ssize_t oled_write(struct file *file,
                          const char __user *buf,
                          size_t count,
                          loff_t *f_pos)
{
    unsigned char *kbuf;
    int ret;

    // SSD1306 128x64 = 1024바이트
    if (count > 1024)
        count = 1024;

    // 커널 메모리 할당
    kbuf = kmalloc(count, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    // 유저 공간 → 커널 공간 복사
    if (copy_from_user(kbuf, buf, count)) {
        kfree(kbuf);
        return -EFAULT;
    }

    // 컬럼 주소 설정 (0~127)
    oled_i2c_write_cmd(0x21);
    oled_i2c_write_cmd(0);
    oled_i2c_write_cmd(127);

    // 페이지 주소 설정 (0~7)
    oled_i2c_write_cmd(0x22);
    oled_i2c_write_cmd(0);
    oled_i2c_write_cmd(7);

    // 화면 데이터 전송
    ret = oled_i2c_write_data(kbuf, count);

    kfree(kbuf);
    return ret;
}

/*
 * 파일 오퍼레이션 구조체
 */
static struct file_operations oled_fops = {
    .owner   = THIS_MODULE,
    .open    = oled_open,
    .release = oled_release,
    .write   = oled_write,
};

/*
 * 모듈 로드 시 실행
 */
static int __init oled_driver_init(void)
{
    int ret;

    // I2C 슬레이브 정보 (이름 + 주소)
    struct i2c_board_info board_info = {
        I2C_BOARD_INFO("ssd1306", OLED_I2C_ADDR)
    };

    pr_info("OLED Driver: Initializing\n");

    // 문자 디바이스 등록
    ret = register_chrdev(DRIVER_MAJOR, DRIVER_NAME, &oled_fops);
    if (ret < 0)
        return ret;

    // I2C 버스 어댑터 얻기
    oled_i2c_adapter = i2c_get_adapter(I2C_BUS_NUM);
    if (!oled_i2c_adapter) {
        unregister_chrdev(DRIVER_MAJOR, DRIVER_NAME);
        return -ENODEV;
    }

    // I2C 클라이언트 생성 (OLED 장치 등록)
    oled_i2c_client = i2c_new_client_device(oled_i2c_adapter, &board_info);
    if (!oled_i2c_client) {
        i2c_put_adapter(oled_i2c_adapter);
        unregister_chrdev(DRIVER_MAJOR, DRIVER_NAME);
        return -ENODEV;
    }

    pr_info("OLED Driver: /dev/%s ready\n", DRIVER_NAME);
    return 0;
}

/*
 * 모듈 제거 시 실행
 */
static void __exit oled_driver_exit(void)
{
    if (oled_i2c_client)
        i2c_unregister_device(oled_i2c_client);

    if (oled_i2c_adapter)
        i2c_put_adapter(oled_i2c_adapter);

    unregister_chrdev(DRIVER_MAJOR, DRIVER_NAME);
    pr_info("OLED Driver: Exited\n");
}

// 모듈 진입/종료 함수 등록
module_init(oled_driver_init);
module_exit(oled_driver_exit);

// 모듈 정보
MODULE_LICENSE("GPL");
MODULE_AUTHOR("User");
MODULE_DESCRIPTION("SSD1306 OLED I2C Driver for /dev/my_oled");
