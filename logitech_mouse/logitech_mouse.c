#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/hid.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/time.h>

#define BUFFER_SIZE 256
#define DEVICE_NAME "logitech_mouse"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hoai Son - Thanh Nhan");
MODULE_DESCRIPTION("Driver Logitech 046d:c077");

// Cấu trúc dữ liệu cho sự kiện chuột
struct mouse_event {
    long long timestamp_sec; // Giây
    long timestamp_nsec;     // Nano giây
    int type;                // 0: MOVE, 1: CLICK, 2: WHEEL
    int x;                   // Tọa độ x
    int y;                   // Tọa độ y
    int button;              // 0: LEFT, 1: RIGHT, 2: MIDDLE
    int action;              // 0: RELEASE, 1: PRESS
    int wheel_value;         // Giá trị cuộn
};

static dev_t dev_num;
static struct cdev cdev;
static struct class *mouse_class;
static struct device *mouse_device;

static char buffer[BUFFER_SIZE * sizeof(struct mouse_event)];
static int buffer_head = 0;
static int buffer_tail = 0;
static spinlock_t buffer_lock;
static wait_queue_head_t read_queue;


// Thêm sự kiện vào buffer
static void enqueue_event(struct mouse_event *event) {
    spin_lock(&buffer_lock);
    if ((buffer_head + 1) % BUFFER_SIZE == buffer_tail) {
        spin_unlock(&buffer_lock); // Buffer đầy
        return;
    }
    memcpy(&buffer[buffer_head * sizeof(struct mouse_event)], event, sizeof(struct mouse_event));
    buffer_head = (buffer_head + 1) % BUFFER_SIZE;
    spin_unlock(&buffer_lock);
    wake_up_interruptible(&read_queue);
}

// Hàm mở thiết bị
static int mouse_open(struct inode *inode, struct file *file) {
    return 0;
}

// Hàm đọc dữ liệu từ thiết bị
static ssize_t mouse_read(struct file *file, char __user *user_buffer, size_t size, loff_t *offset) {
    struct mouse_event event;
    int ret;

    if (wait_event_interruptible(read_queue, buffer_head != buffer_tail)) {
        return -ERESTARTSYS;
    }

    spin_lock(&buffer_lock);
    if (buffer_head == buffer_tail) {
        spin_unlock(&buffer_lock);
        return 0; // Không có dữ liệu
    }
    memcpy(&event, &buffer[buffer_tail * sizeof(struct mouse_event)], sizeof(struct mouse_event));
    buffer_tail = (buffer_tail + 1) % BUFFER_SIZE;
    spin_unlock(&buffer_lock);

    if (size < sizeof(struct mouse_event)) {
        return -EINVAL;
    }

    ret = copy_to_user(user_buffer, &event, sizeof(struct mouse_event));
    if (ret) {
        return -EFAULT;
    }

    return sizeof(struct mouse_event);
}

// Hàm đóng thiết bị
static int mouse_release(struct inode *inode, struct file *file) {
    return 0;
}

// Định nghĩa các hàm thao tác với thiết bị
static const struct file_operations mouse_fops = {
    .owner = THIS_MODULE,
    .open = mouse_open,
    .read = mouse_read,
    .release = mouse_release,
};

// Xử lý sự kiện HID
static int mouse_event(struct hid_device *hdev, struct hid_field *field, struct hid_usage *usage, __s32 value)
{
    struct timespec64 ts;
    ktime_get_real_ts64(&ts);
    struct mouse_event event = {0};

    event.timestamp_sec = ts.tv_sec;
    event.timestamp_nsec = ts.tv_nsec;

    if (usage->type == EV_REL) {
        if (usage->code == REL_X || usage->code == REL_Y) {
            event.type = 0; // MOVE
            if (usage->code == REL_X) event.x = value;
            else event.y = value;
        } else if (usage->code == REL_WHEEL) {
            event.type = 2; // WHEEL
            event.wheel_value = value;
        }
    } else if (usage->type == EV_KEY) {
        if (usage->code == BTN_LEFT || usage->code == BTN_RIGHT || usage->code == BTN_MIDDLE) {
            event.type = 1; // CLICK
            event.button = (usage->code == BTN_LEFT) ? 0 : (usage->code == BTN_RIGHT) ? 1 : 2;
            event.action = value; // 1: PRESS, 0: RELEASE
        }
    }

    enqueue_event(&event);
    return 0;
}

static const struct hid_device_id mouse_id_table[] = {
    { HID_USB_DEVICE(0x046d, 0xc077) },
    { }
};
MODULE_DEVICE_TABLE(hid, mouse_id_table);

static int mouse_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
    int ret;
    hid_info(hdev, "Connected to Logitech 046d:c077\n");
    ret = hid_parse(hdev);
    if (ret) return ret;
    ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
    if (ret) return ret;
    return 0;
}

static void mouse_remove(struct hid_device *hdev)
{
    hid_info(hdev, "Disconnected to Logitech 046d:c077\n");
    hid_hw_stop(hdev);
}

static struct hid_driver mouse_driver = {
    .name = "logitech_mouse",
    .id_table = mouse_id_table,
    .probe = mouse_probe,
    .remove = mouse_remove,
    .event = mouse_event,
};

// Khởi tạo driver
static int __init mouse_init(void) {
    int ret;

    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0) return ret;

    // Sửa lại chỉ truyền 1 tham số cho class_create
    mouse_class = class_create(DEVICE_NAME);
    if (IS_ERR(mouse_class)) {
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(mouse_class);
    }

    cdev_init(&cdev, &mouse_fops);
    ret = cdev_add(&cdev, dev_num, 1);
    if (ret < 0) {
        class_destroy(mouse_class);
        unregister_chrdev_region(dev_num, 1);
        return ret;
    }

    mouse_device = device_create(mouse_class, NULL, dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(mouse_device)) {
        cdev_del(&cdev);
        class_destroy(mouse_class);
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(mouse_device);
    }

    spin_lock_init(&buffer_lock);
    init_waitqueue_head(&read_queue);

    ret = hid_register_driver(&mouse_driver);
    if (ret) {
        device_destroy(mouse_class, dev_num);
        cdev_del(&cdev);
        class_destroy(mouse_class);
        unregister_chrdev_region(dev_num, 1);
    }

    return ret;
}

// Thoát driver
static void __exit mouse_exit(void) {
    hid_unregister_driver(&mouse_driver);
    device_destroy(mouse_class, dev_num);
    cdev_del(&cdev);
    class_destroy(mouse_class);
    unregister_chrdev_region(dev_num, 1);
}

module_init(mouse_init);
module_exit(mouse_exit);