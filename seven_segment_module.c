#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/delay.h>

#define FIRST_GPIO 2
#define NUM_GPIOS 7
#define DOT_GPIO 9
#define DEVICE_NAME "seven_segment_display"

static int major_number;
static struct class* seven_segment_class = NULL;
static struct device* seven_segment_device = NULL;
static enum {OFF, NUMBER, ANIMATION} mode = OFF;
static struct task_struct *dot_thread = NULL;
static struct task_struct *animation_thread = NULL;
static int current_number = -1;

static int segment_pins[NUM_GPIOS] = {2, 3, 4, 5, 6, 7, 8};
static bool segments_off[NUM_GPIOS] = {true, true, true, true, true, true, true};
static bool segments_number[10][7] = {
        {false, false, false, false, false, false, true},   // 0
        {true, false, false, true, true, true, true},       // 1
        {false, false, true, false, false, true, false},    // 2
        {false, false, false, false, true, true, false},    // 3
        {true, false, false, true, true, false, false},     // 4
        {false, true, false, false, true, false, false},    // 5
        {false, true, false, false, false, false, false},   // 6
        {false, false, false, true, true, true, true},      // 7
        {false, false, false, false, false, false, false},  // 8
        {false, false, false, false, true, false, false}    // 9
};

static bool segments_animation[8][7] = {
        {false, true, true, true, true, true, true},
        {true, false, true, true, true, true, true},
        {true, true, true, true, true, true, false},
        {true, true, true, true, false, true, true},
        {true, true, true, false, true, true, true},
        {true, true, false, true, true, true, true},
        {true, true, true, true, true, true, false},
        {true, true, true, true, true, false, true}
};

static int device_open(struct inode *, struct file *);
static int device_release(struct inode *, struct file *);
static ssize_t device_read(struct file *, char *, size_t, loff_t *);
static ssize_t device_write(struct file *, const char *, size_t, loff_t *);

static struct file_operations fops = {
        .open = device_open,
        .read = device_read,
        .write = device_write,
        .release = device_release,
};



int set_segment_values(bool values[]) {
    int i;
    for (i = 0; i < NUM_GPIOS; i++) {
        gpio_set_value(segment_pins[i], values[i]);
    }
    return 0;
}



void display_number(int num) {
    bool values[NUM_GPIOS];
    int i;
    for (i = 0; i < 7; i++) {
        values[i] = segments_number[num][i];
    }
    set_segment_values(values);
}



int dot_blinking_thread(void *data) {
    while (!kthread_should_stop()) {
        // Toggle the dot state
        gpio_set_value(DOT_GPIO, true);
        msleep(500);
        gpio_set_value(DOT_GPIO, false);
        msleep(500);
    }
    return 0;
}



int animation_thread_function(void *data) {
    int i;
    while (!kthread_should_stop()) {
        for (i = 0; i < 8; i++) {
            set_segment_values(segments_animation[i]);
            msleep(100); // Adjust as needed
        }
    }
    return 0;
}



static int __init sevenseg_init(void) {
    int i;

    // Register device
    major_number = register_chrdev(0, DEVICE_NAME, &fops);
    if (major_number < 0) {
        printk(KERN_ALERT "seven_segment_display: failed to register a major number\n");
        return major_number;
    }

    seven_segment_class = class_create(THIS_MODULE, DEVICE_NAME);
    if (IS_ERR(seven_segment_class)) {
        unregister_chrdev(major_number, DEVICE_NAME);
        printk(KERN_ALERT "seven_segment_display: failed to register device class\n");
        return PTR_ERR(seven_segment_class);
    }

    seven_segment_device = device_create(seven_segment_class, NULL, MKDEV(major_number, 0), NULL, DEVICE_NAME);
    if (IS_ERR(seven_segment_device)) {
        class_destroy(seven_segment_class);
        unregister_chrdev(major_number, DEVICE_NAME);
        printk(KERN_ALERT "seven_segment_display: failed to create the device\n");
        return PTR_ERR(seven_segment_device);
    }

    // Initialize GPIOs and turn off all segments (set all high for common anode)
    for (i = 0; i < NUM_GPIOS; i++) {
        gpio_request(segment_pins[i], "sysfs");
        gpio_direction_output(segment_pins[i], segments_off[i]);
        gpio_export(segment_pins[i], false);
    }

    gpio_request(DOT_GPIO, "sysfs");
    gpio_direction_output(DOT_GPIO, true);
    gpio_export(DOT_GPIO, false);

    // Start the dot blinking thread
    dot_thread = kthread_run(dot_blinking_thread, NULL, "dot_blinking_thread");
    if (IS_ERR(dot_thread)) {
        printk(KERN_ALERT "seven_segment_display: failed to create the dot blinking thread\n");
        return PTR_ERR(dot_thread);
    }

    printk(KERN_INFO "seven_segment_display: device class created correctly\n");
    return 0;
}



static void __exit sevenseg_exit(void) {
    if (dot_thread) {
        kthread_stop(dot_thread);
    }
    if (animation_thread) {
        kthread_stop(animation_thread);
    }

    set_segment_values(segments_off);
    gpio_set_value(DOT_GPIO, true);

    for (int i = 0; i < NUM_GPIOS; i++) {
        gpio_unexport(segment_pins[i]);
        gpio_free(segment_pins[i]);
    }

    gpio_unexport(DOT_GPIO);
    gpio_free(DOT_GPIO);

    device_destroy(seven_segment_class, MKDEV(major_number, 0));
    class_unregister(seven_segment_class);
    class_destroy(seven_segment_class);
    unregister_chrdev(major_number, DEVICE_NAME);
    printk(KERN_INFO "seven_segment_display: bye-bye!\n");
}



static int device_open(struct inode *inodep, struct file *filep) {
    return 0;
}



static ssize_t device_write(struct file *filep, const char *buffer, size_t len, loff_t *offset) {
    char msg[20] = {0};
    if (len > 19) {
        return -EINVAL; // Message too long
    }
    if (copy_from_user(msg, buffer, len)) {
        return -EFAULT; // Copy failed
    }

    if (isdigit(msg[0])) {
        int num = msg[0] - '0';
        if (num >= 0 && num <= 9) {
            if (animation_thread) {
                kthread_stop(animation_thread);
                animation_thread = NULL;
            }
            display_number(num);
            mode = NUMBER;
            current_number = num;
        }
    } else if (strncmp(msg, "off", 3) == 0) {
        if (animation_thread) {
            kthread_stop(animation_thread);
            animation_thread = NULL;
        }
        set_segment_values(segments_off);
        mode = OFF;
    } else if (strncmp(msg, "animation", 9) == 0) {
        if (animation_thread) {
            kthread_stop(animation_thread);
        }
        animation_thread = kthread_run(animation_thread_function, NULL, "animation_thread");
        if (IS_ERR(animation_thread)) {
            printk(KERN_ALERT "seven_segment_display: Failed to start the animation thread\n");
            return PTR_ERR(animation_thread);
        }
        mode = ANIMATION;
    }

    return len;
}



static ssize_t device_read(struct file *filep, char *buffer, size_t len, loff_t *offset) {
    char msg[20];
    int msg_len;

    switch (mode) {
        case OFF:
            msg_len = snprintf(msg, 20, "Off\n");
            break;
        case NUMBER:
            msg_len = snprintf(msg, 20, "Number: %d\n", current_number);
            break;
        case ANIMATION:
            msg_len = snprintf(msg, 20, "Animation\n");
            break;
        default:
            msg_len = snprintf(msg, 20, "Unknown\n");
            break;
    }

    if (*offset >= msg_len) {
        return 0; // EOF
    }

    if (len > msg_len - *offset) {
        len = msg_len - *offset;
    }

    if (copy_to_user(buffer, msg + *offset, len)) {
        return -EFAULT;
    }

    *offset += len;
    return len;
}



static int device_release(struct inode *inodep, struct file *filep) {
    return 0;
}



module_init(sevenseg_init);
module_exit(sevenseg_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mihhail Tsulinda");
MODULE_DESCRIPTION("A Linux LED driver for 7-segment display");
MODULE_VERSION("0.1");