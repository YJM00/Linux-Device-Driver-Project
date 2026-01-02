// dht11_driver.c
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/errno.h>
#include <linux/interrupt.h>

#define DEV_NAME "dht11_driver"

// ====== DHT11 DATA GPIO (BCM 번호) ======
#define DHT_GPIO 4   // 필요하면 바꿔라 (예: BCM17이면 17)

// DHT11은 너무 자주 읽으면 안 됨(권장 1초 이상 간격)
#define MIN_READ_INTERVAL_MS 1000

// 타임아웃(마이크로초 단위) - 무한루프 방지
#define TIMEOUT_US 200

typedef struct {
    int hum;   // 습도
    int temp;  // 온도
} dht11_info_t;

static dev_t dht_dev;
static struct cdev dht_cdev;
static struct class *dht_class;
static struct device *dht_device;

static unsigned long last_read_jiffies; // 마지막 성공/시도 시간(간격 제한)

// ====== 유틸: 특정 레벨이 될 때까지 기다리기 ======
static int wait_for_level(int gpio, int level, int timeout_us)
{
    int t = 0;
    while (gpio_get_value(gpio) != level) {
        udelay(1);
        if (++t >= timeout_us)
            return -ETIMEDOUT;
    }
    return 0;
}

// ====== 핵심: DHT11 한 번 읽기 ======
// out[0]=hum_int, out[1]=hum_dec, out[2]=temp_int, out[3]=temp_dec, out[4]=checksum
static int dht11_read_raw(u8 out[5])
{
    int i, bit;
    unsigned long flags;

    u8 data[5] = {0,0,0,0,0};

    // 타이밍 민감 구간: 인터럽트로 깨지면 실패율 폭증
    local_irq_save(flags);

    // 1) MCU(Start signal): DATA를 출력으로 LOW 18ms 이상 유지
    gpio_direction_output(DHT_GPIO, 0);
    mdelay(20);                 // 18ms 이상 (여유로 20ms)
    gpio_set_value(DHT_GPIO, 1);
    udelay(30);                 // 20~40us 정도 HIGH
    gpio_direction_input(DHT_GPIO); // 입력 전환

    // 2) 센서 응답: LOW(약80us) -> HIGH(약80us)
    if (wait_for_level(DHT_GPIO, 0, TIMEOUT_US) < 0) { local_irq_restore(flags); return -EIO; }
    if (wait_for_level(DHT_GPIO, 1, TIMEOUT_US) < 0) { local_irq_restore(flags); return -EIO; }
    if (wait_for_level(DHT_GPIO, 0, TIMEOUT_US) < 0) { local_irq_restore(flags); return -EIO; }

    // 3) 데이터 40비트 읽기
    // 각 비트: LOW(약50us) -> HIGH(26~28us=0 / 70us=1)
    for (i = 0; i < 40; i++) {
        int high_len = 0;

        // LOW 시작(이미 LOW일 수 있지만 안정적으로 기다림)
        if (wait_for_level(DHT_GPIO, 0, TIMEOUT_US) < 0) { local_irq_restore(flags); return -EIO; }

        // HIGH 시작
        if (wait_for_level(DHT_GPIO, 1, TIMEOUT_US) < 0) { local_irq_restore(flags); return -EIO; }

        // HIGH 지속 시간 측정 (us 단위)
        while (gpio_get_value(DHT_GPIO) == 1) {
            udelay(1);
            if (++high_len >= TIMEOUT_US) { local_irq_restore(flags); return -EIO; }
        }

        // 판정: 대략 40us 기준으로 0/1 구분 (환경 따라 30~50us 조절 가능)
        bit = (high_len > 40) ? 1 : 0;

        // i번째 비트를 data[]에 채우기 (MSB first)
        data[i/8] <<= 1;
        data[i/8] |= bit;
    }

    local_irq_restore(flags);

    // 바이트 5개 복사
    for (i = 0; i < 5; i++)
        out[i] = data[i];

    // 4) 체크섬 검사
    if ((u8)(data[0] + data[1] + data[2] + data[3]) != data[4])
        return -EBADMSG;

    return 0;
}

// ====== file ops: read ======
static ssize_t dht_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    u8 raw[5];
    int ret;
    dht11_info_t info;

    // read를 여러 번 호출해도 한번만 주고 끝내고 싶으면(표준적인 char read):
    // 오프셋이 0이 아니면 EOF 처리
    if (*ppos != 0)
        return 0;

    // 1초 간격 제한 (너무 자주 읽으면 센서가 값 안 줌/에러남)
    if (time_before(jiffies, last_read_jiffies + msecs_to_jiffies(MIN_READ_INTERVAL_MS)))
        return -EAGAIN;

    last_read_jiffies = jiffies;

    ret = dht11_read_raw(raw);
    if (ret < 0)
        return ret;

    // DHT11: [0]=습도정수, [1]=습도소수(보통 0), [2]=온도정수, [3]=온도소수(보통 0)
    info.hum  = (int)raw[0];
    info.temp = (int)raw[2];

    // 유저가 구조체 크기보다 적게 읽겠다고 하면 최소한만 보내는건 애매해서 에러 처리
    if (count < sizeof(dht11_info_t))
        return -EINVAL;

    if (copy_to_user(buf, &info, sizeof(dht11_info_t)))
        return -EFAULT;

    *ppos += sizeof(dht11_info_t);
    return sizeof(dht11_info_t);
}

static int dht_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int dht_release(struct inode *inode, struct file *file)
{
    return 0;
}

static const struct file_operations dht_fops = {
    .owner   = THIS_MODULE,
    .open    = dht_open,
    .release = dht_release,
    .read    = dht_read,
};

// ====== init/exit ======
static int __init dht_init(void)
{
    int ret;

    pr_info("DHT11: init (GPIO=%d)\n", DHT_GPIO);

    // 1) GPIO 확보
    ret = gpio_request(DHT_GPIO, "dht11_data");
    if (ret) {
        pr_err("DHT11: gpio_request failed\n");
        return ret;
    }
    gpio_direction_input(DHT_GPIO); // 기본은 입력

    // 2) chrdev 번호 할당
    ret = alloc_chrdev_region(&dht_dev, 0, 1, DEV_NAME);
    if (ret < 0) {
        pr_err("DHT11: alloc_chrdev_region failed\n");
        gpio_free(DHT_GPIO);
        return ret;
    }

    // 3) cdev 등록
    cdev_init(&dht_cdev, &dht_fops);
    dht_cdev.owner = THIS_MODULE;

    ret = cdev_add(&dht_cdev, dht_dev, 1);
    if (ret < 0) {
        pr_err("DHT11: cdev_add failed\n");
        unregister_chrdev_region(dht_dev, 1);
        gpio_free(DHT_GPIO);
        return ret;
    }

    // 4) /dev 자동 생성(class/device)
    dht_class = class_create(THIS_MODULE, DEV_NAME);
    if (IS_ERR(dht_class)) {
        pr_err("DHT11: class_create failed\n");
        cdev_del(&dht_cdev);
        unregister_chrdev_region(dht_dev, 1);
        gpio_free(DHT_GPIO);
        return PTR_ERR(dht_class);
    }

    dht_device = device_create(dht_class, NULL, dht_dev, NULL, DEV_NAME);
    if (IS_ERR(dht_device)) {
        pr_err("DHT11: device_create failed\n");
        class_destroy(dht_class);
        cdev_del(&dht_cdev);
        unregister_chrdev_region(dht_dev, 1);
        gpio_free(DHT_GPIO);
        return PTR_ERR(dht_device);
    }

    last_read_jiffies = 0;
    pr_info("DHT11: /dev/%s created (major=%d minor=%d)\n",
            DEV_NAME, MAJOR(dht_dev), MINOR(dht_dev));
    pr_info("DHT11: read returns struct {int hum; int temp}\n");

    return 0;
}

static void __exit dht_exit(void)
{
    device_destroy(dht_class, dht_dev);
    class_destroy(dht_class);
    cdev_del(&dht_cdev);
    unregister_chrdev_region(dht_dev, 1);
    gpio_free(DHT_GPIO);

    pr_info("DHT11: exit\n");
}

module_init(dht_init);
module_exit(dht_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("JongMin");
MODULE_DESCRIPTION("DHT11 GPIO Bitbang Driver (/dev/dht11_driver)");

