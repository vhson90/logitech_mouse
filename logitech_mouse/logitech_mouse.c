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
#include <linux/hrtimer.h>

#define BUFFER_SIZE 256
#define DEVICE_NAME "logitech_mouse"
#define REPORT_INTERVAL_MS 8 // 8ms = 125Hz
#define REPORT_INTERVAL_NS (REPORT_INTERVAL_MS * 1000000L)

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hoai Son & Trong Nhan");
MODULE_DESCRIPTION("Driver Logitech 046d:c077 with 125Hz move reporting");

struct mouse_event {
    long long timestamp_sec;
    long timestamp_nsec;
    int type;           // 0: MOVE, 1: CLICK, 2: WHEEL
    int x;              // Tọa độ x tương đối
    int y;              // Tọa độ y tương đối
    int button;         // 0: LEFT, 1: RIGHT, 2: MIDDLE
    int action;         // 0: RELEASE, 1: PRESS
    int wheel_value;    // Giá trị cuộn
};

static dev_t dev_num;
static struct cdev cdev;
static struct class *mouse_class;
static struct device *mouse_device;
static char *buffer;
static int buffer_head = 0;
static int buffer_tail = 0;
static spinlock_t buffer_lock;
static wait_queue_head_t read_queue;

static struct hrtimer move_timer;
static struct mouse_event pending_move = {0};
static int has_x = 0, has_y = 0;
static int last_value[3] = {0}; // Trạng thái nút trước đó

static void enqueue_event(struct mouse_event *event) {
    spin_lock(&buffer_lock);
    if ((buffer_head + 1) % BUFFER_SIZE == buffer_tail) {
        pr_warn("Buffer full, event dropped\n");
        spin_unlock(&buffer_lock);
        return;
    }
    memcpy(&buffer[buffer_head * sizeof(struct mouse_event)], event, sizeof(struct mouse_event));
    buffer_head = (buffer_head + 1) % BUFFER_SIZE;
    spin_unlock(&buffer_lock);
    wake_up_interruptible(&read_queue);
}

static int mouse_open(struct inode *inode, struct file *file) {
    return 0;
}

static ssize_t mouse_read(struct file *file, char __user *user_buffer, size_t size, loff_t *offset) {
    struct mouse_event event;
    int ret;

    if (wait_event_interruptible(read_queue, buffer_head != buffer_tail)) {
        return -ERESTARTSYS;
    }

    spin_lock(&buffer_lock);
    memcpy(&event, &buffer[buffer_tail * sizeof(struct mouse_event)], sizeof(struct mouse_event));
    spin_unlock(&buffer_lock);

    if (size < sizeof(struct mouse_event)) {
        return -EINVAL;
    }

    ret = copy_to_user(user_buffer, &event, sizeof(struct mouse_event));
    if (ret) {
        return -EFAULT;
    }

    spin_lock(&buffer_lock);
    buffer_tail = (buffer_tail + 1) % BUFFER_SIZE;
    spin_unlock(&buffer_lock);

    return sizeof(struct mouse_event);
}

static int mouse_release(struct inode *inode, struct file *file) {
    return 0;
}

static const struct file_operations mouse_fops = {
    .owner = THIS_MODULE,
    .open = mouse_open,
    .read = mouse_read,
    .release = mouse_release,
};

// Hàm callback của timer để gửi báo cáo MOVE
static enum hrtimer_restart move_timer_callback(struct hrtimer *timer) {
    struct timespec64 ts;
    ktime_get_real_ts64(&ts);

    if (has_x || has_y) {
        pending_move.timestamp_sec = ts.tv_sec;
        pending_move.timestamp_nsec = ts.tv_nsec;
        pending_move.type = 0; // MOVE
        enqueue_event(&pending_move);
        memset(&pending_move, 0, sizeof(struct mouse_event));
        has_x = has_y = 0;
    }

    hrtimer_forward_now(timer, ktime_set(0, REPORT_INTERVAL_NS));
    return HRTIMER_RESTART;
}

static int mouse_event(struct hid_device *hdev, struct hid_field *field, struct hid_usage *usage, __s32 value)
{
    struct timespec64 ts;
    ktime_get_real_ts64(&ts);

    if (usage->type == EV_REL) {
        if (usage->code == REL_X && value != 0) {
            pending_move.x += value; // Tích lũy delta_x
            has_x = 1;
        } else if (usage->code == REL_Y && value != 0) {
            pending_move.y += value; // Tích lũy delta_y
            has_y = 1;
        } else if ((usage->code == REL_WHEEL || usage->code == REL_WHEEL_HI_RES) && value != 0) {
            if (has_x || has_y) { // Gửi MOVE trước nếu có
                pending_move.timestamp_sec = ts.tv_sec;
                pending_move.timestamp_nsec = ts.tv_nsec;
                pending_move.type = 0;
                enqueue_event(&pending_move);
                memset(&pending_move, 0, sizeof(struct mouse_event));
                has_x = has_y = 0;
            }
            struct mouse_event wheel_event = {0};
            wheel_event.timestamp_sec = ts.tv_sec;
            wheel_event.timestamp_nsec = ts.tv_nsec;
            wheel_event.type = 2; // WHEEL
            wheel_event.wheel_value = value;
            enqueue_event(&wheel_event);
        }
    } else if (usage->type == EV_KEY) {
        if (usage->code == BTN_LEFT || usage->code == BTN_RIGHT || usage->code == BTN_MIDDLE) {
            int button_idx = (usage->code == BTN_LEFT) ? 0 : (usage->code == BTN_RIGHT) ? 1 : 2;
            if (value != last_value[button_idx]) { // Chỉ ghi khi trạng thái thay đổi
                if (has_x || has_y) { // Gửi MOVE trước nếu có
                    pending_move.timestamp_sec = ts.tv_sec;
                    pending_move.timestamp_nsec = ts.tv_nsec;
                    pending_move.type = 0;
                    enqueue_event(&pending_move);
                    memset(&pending_move, 0, sizeof(struct mouse_event));
                    has_x = has_y = 0;
                }
                struct mouse_event click_event = {0};
                click_event.timestamp_sec = ts.tv_sec;
                click_event.timestamp_nsec = ts.tv_nsec;
                click_event.type = 1; // CLICK
                click_event.button = button_idx;
                click_event.action = value;
                enqueue_event(&click_event);
                last_value[button_idx] = value;
            }
        }
    }

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

    // Khởi tạo timer cho báo cáo 125Hz
    hrtimer_init(&move_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    move_timer.function = move_timer_callback;
    hrtimer_start(&move_timer, ktime_set(0, REPORT_INTERVAL_NS), HRTIMER_MODE_REL);

    return 0;
}

static void mouse_remove(struct hid_device *hdev)
{
    hid_info(hdev, "Disconnected from Logitech 046d:c077\n");
    hrtimer_cancel(&move_timer);
    hid_hw_stop(hdev);
}

static struct hid_driver mouse_driver = {
    .name = "logitech_mouse",
    .id_table = mouse_id_table,
    .probe = mouse_probe,
    .remove = mouse_remove,
    .event = mouse_event,
};

static int __init mouse_init(void) {
    int ret;

    buffer = kmalloc(BUFFER_SIZE * sizeof(struct mouse_event), GFP_KERNEL);
    if (!buffer) {
        return -ENOMEM;
    }

    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0) goto err_free_buffer;

    mouse_class = class_create(DEVICE_NAME);
    if (IS_ERR(mouse_class)) {
        ret = PTR_ERR(mouse_class);
        goto err_free_region;
    }

    cdev_init(&cdev, &mouse_fops);
    ret = cdev_add(&cdev, dev_num, 1);
    if (ret < 0) goto err_destroy_class;

    mouse_device = device_create(mouse_class, NULL, dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(mouse_device)) {
        ret = PTR_ERR(mouse_device);
        goto err_del_cdev;
    }

    spin_lock_init(&buffer_lock);
    init_waitqueue_head(&read_queue);

    ret = hid_register_driver(&mouse_driver);
    if (ret) goto err_destroy_device;

    return 0;

err_destroy_device:
    device_destroy(mouse_class, dev_num);
err_del_cdev:
    cdev_del(&cdev);
err_destroy_class:
    class_destroy(mouse_class);
err_free_region:
    unregister_chrdev_region(dev_num, 1);
err_free_buffer:
    kfree(buffer);
    return ret;
}

static void __exit mouse_exit(void) {
    hid_unregister_driver(&mouse_driver);
    device_destroy(mouse_class, dev_num);
    cdev_del(&cdev);
    class_destroy(mouse_class);
    unregister_chrdev_region(dev_num, 1);
    kfree(buffer);
}

module_init(mouse_init);
module_exit(mouse_exit);