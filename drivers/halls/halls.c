#include <linux/gpio/consumer.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/semaphore.h>
#include <linux/workqueue.h>
#include <linux/pm.h>
#include <linux/pm_wakeup.h>
#include <linux/halls.h>

#define HALLS_LOG printk

#define LOGTAG "halls "
#define DEF_HALLS_DEVICE_NAME "halls"

#define DEF_HALLS_IOCTRL_READ_HALL_1 1000
#define DEF_HALLS_IOCTRL_READ_HALL_2 2000
#define DEF_HALLS_IOCTRL_FLUSH 3000

#define DEF_HALLS_IOCTRL_STEUP 4000
#define DEF_HALLS_IOCTRL_RELEASE 5000

#define DEF_HALL_IRQ_1_FIRED "h-1"
#define DEF_HALL_IRQ_2_FIRED "h-2"
#define DEF_HALL_FLUSH "flu"

//extern int elliptic_set_hall_state(int state);

struct _hall_device {
	struct miscdevice miscdev;
	struct delayed_work notify_work;
	struct wakeup_source *hall_wakelock;
	struct semaphore sem_irq;
	struct mutex lock;
	struct gpio_desc *gpio_1;
	struct gpio_desc *gpio_2;
	int gpio_1_irq;
	int gpio_2_irq;
	int fired_irq;
	int in_end;
	int in_flush;
	int irqs_requested;
	int hall_status;
};

/* Only one instance is supported: the misc device name is fixed. */
static struct _hall_device *g_st_halls;

static halls_status_notify halls_status_notify_cb = NULL;

void halls_register_halls_status_notify(halls_status_notify halls_cb)
{
	halls_status_notify_cb = halls_cb;
}
EXPORT_SYMBOL(halls_register_halls_status_notify);

void halls_deregister_halls_status_notify(halls_status_notify halls_cb)
{
	halls_status_notify_cb = NULL;
}
EXPORT_SYMBOL(halls_deregister_halls_status_notify);

static void hall_notify_work_func(struct work_struct *work)
{
	struct _hall_device *halls =
		container_of(work, struct _hall_device, notify_work.work);
	int iRet1 = 0, iRet2 = 0;

	iRet1 = gpiod_get_value_cansleep(halls->gpio_1);
	iRet2 = gpiod_get_value_cansleep(halls->gpio_2);
	HALLS_LOG(LOGTAG "fired_irq %d, gpio_1_irq %d, gpio_2_irq %d",
		  halls->fired_irq, halls->gpio_1_irq, halls->gpio_2_irq);
	HALLS_LOG(LOGTAG " iRet1 %d	iRet2 %d", iRet1, iRet2);
	if (iRet1 == 0 && iRet2 == 1) {
		halls->hall_status = 1;
	} else if (iRet1 == 1 && iRet2 == 0) {
		halls->hall_status = 0;
	} else {
		halls->hall_status = 2;
	}

	HALLS_LOG(LOGTAG "hall_notify_work_func notify audio hall_status: %d!",
		  halls->hall_status);
	//elliptic_set_hall_state(halls->hall_status);
	if (halls_status_notify_cb != NULL) {
		halls_status_notify_cb(halls->hall_status);
	}
}

static irqreturn_t halls_irq_handler(int irqno, void *dev_id)
{
	struct _hall_device *halls = dev_id;

	HALLS_LOG(LOGTAG "Entered halls_irq_handler irqno %d\n", irqno);
	__pm_wakeup_event(halls->hall_wakelock, 20);
	halls->fired_irq = irqno;
	up(&halls->sem_irq);
	schedule_delayed_work(&halls->notify_work, 0);
	return IRQ_HANDLED;
}

static int halls_configurate_irq(struct _hall_device *halls,
				 struct gpio_desc *gpio, const char *ch_mark,
				 int *out_irq)
{
	int iRet = 0, in_IRQ = 0;

	in_IRQ = gpiod_to_irq(gpio);
	if (in_IRQ < 0) {
		pr_err("gpiod_to_irq failed for %s! iRet %d\n", ch_mark,
		       in_IRQ);
		*out_irq = 0;
		return in_IRQ;
	}
	HALLS_LOG(LOGTAG "%s IRQ %d\n", ch_mark, in_IRQ);
	iRet = request_irq(in_IRQ, halls_irq_handler,
			   IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, ch_mark,
			   halls);
	if (iRet) {
		pr_err("request_irq failed! iRet %d\n", iRet);
		*out_irq = 0;
		return iRet;
	}
	*out_irq = in_IRQ;
	enable_irq_wake(in_IRQ);

	HALLS_LOG(LOGTAG "request_irq return %d\n", iRet);
	return iRet;
}

static void halls_release_irq(struct _hall_device *halls, int *irq)
{
	if (*irq > 0) {
		disable_irq_wake(*irq);
		free_irq(*irq, halls);
	}
	*irq = 0;
}

static ssize_t halls_read(struct file *file, char __user *buf, size_t count,
			  loff_t *ppos)
{
	struct _hall_device *halls = g_st_halls;
	int iRead = 0;

	HALLS_LOG(LOGTAG "Entereed halls_read  count  %d", (int)count);
	if (down_interruptible(&halls->sem_irq)) {
		HALLS_LOG(LOGTAG "down_interruptible failed!");
	}
	if (halls->in_end == 1) {
		HALLS_LOG(LOGTAG "Ending... ");
		return -EPERM;
	}
	HALLS_LOG(LOGTAG "fired_irq %d, gpio_1_irq %d, gpio_2_irq %d",
		  halls->fired_irq, halls->gpio_1_irq, halls->gpio_2_irq);
	if (halls->in_flush == 1) {
		halls->in_flush = 0;
		iRead = copy_to_user(buf, DEF_HALL_FLUSH,
				     min(count, strlen(DEF_HALL_FLUSH)));
		HALLS_LOG(LOGTAG "End of copy_to_user flash... ");
	} else if (halls->fired_irq == halls->gpio_1_irq) {
		iRead = copy_to_user(buf, DEF_HALL_IRQ_1_FIRED,
				     min(count, strlen(DEF_HALL_IRQ_1_FIRED)));
		HALLS_LOG(LOGTAG "End of copy_to_user 1... ");
	} else if (halls->fired_irq == halls->gpio_2_irq) {
		iRead = copy_to_user(buf, DEF_HALL_IRQ_2_FIRED,
				     min(count, strlen(DEF_HALL_IRQ_2_FIRED)));
		HALLS_LOG(LOGTAG "End of copy_to_user 2... ");
	}
	return iRead;
}

static long halls_in_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	struct _hall_device *halls = g_st_halls;
	int iRet = 0;
	void __user *p_ReadValue = (void __user *)arg;
	char ch_data[10] = { 0 };

	HALLS_LOG(LOGTAG "Enter halls_in_ioctl  cmd %d", cmd);
	switch (cmd) {
	case DEF_HALLS_IOCTRL_READ_HALL_1: {
		iRet = gpiod_get_value_cansleep(halls->gpio_1);
		HALLS_LOG(LOGTAG "Read HALL_1 %d", iRet);
		snprintf(ch_data, sizeof(ch_data), "%d", iRet);
		iRet = copy_to_user(p_ReadValue, ch_data, strlen(ch_data));
		break;
	}
	case DEF_HALLS_IOCTRL_READ_HALL_2: {
		iRet = gpiod_get_value_cansleep(halls->gpio_2);
		HALLS_LOG(LOGTAG "Read HALL_2 %d", iRet);
		snprintf(ch_data, sizeof(ch_data), "%d", iRet);
		iRet = copy_to_user(p_ReadValue, ch_data, strlen(ch_data));
		break;
	}
	case DEF_HALLS_IOCTRL_FLUSH: {
		halls->in_flush = 1;
		up(&halls->sem_irq);
		schedule_delayed_work(&halls->notify_work, 0);
		break;
	}
	case DEF_HALLS_IOCTRL_STEUP: {
		mutex_lock(&halls->lock);
		if (!halls->irqs_requested) {
			halls->in_end = 0;
			sema_init(&halls->sem_irq, 0);
			iRet = halls_configurate_irq(halls, halls->gpio_1,
						     "hall-1",
						     &halls->gpio_1_irq);
			if (iRet)
				pr_err("halls_configurate_irq failed for hall-1! iRet %d\n",
				       iRet);
			halls->fired_irq = halls->gpio_1_irq;

			iRet = halls_configurate_irq(halls, halls->gpio_2,
						     "hall-2",
						     &halls->gpio_2_irq);
			if (iRet)
				pr_err("halls_configurate_irq failed for hall-2! iRet %d\n",
				       iRet);

			HALLS_LOG(LOGTAG "gpio_1_irq %d, gpio_2_irq %d\n",
				  halls->gpio_1_irq, halls->gpio_2_irq);
			halls->irqs_requested = 1;
		}
		mutex_unlock(&halls->lock);
		break;
	}
	case DEF_HALLS_IOCTRL_RELEASE: {
		mutex_lock(&halls->lock);
		halls->in_end = 1;
		up(&halls->sem_irq);
		halls_release_irq(halls, &halls->gpio_1_irq);
		halls_release_irq(halls, &halls->gpio_2_irq);
		halls->irqs_requested = 0;
		mutex_unlock(&halls->lock);
		break;
	}
	default:
		iRet = -1;
	}
	HALLS_LOG(LOGTAG "read value cmd %d, iRet %d", cmd, iRet);
	return iRet;
}

static int halls_flush(struct file *file, fl_owner_t id)
{
	HALLS_LOG(LOGTAG " Entered halls_flush \n");
	return 0;
}

static int halls_open(struct inode *node, struct file *file)
{
	int iRet = 0;

	return iRet;
}

static const struct file_operations halls_fops = {
	.owner = THIS_MODULE,
	.read = halls_read,
	.unlocked_ioctl = halls_in_ioctl,
	.flush = halls_flush,
	.open = halls_open,
};

static int halls_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct _hall_device *halls;
	int ret;

	if (g_st_halls) {
		dev_err(dev, "only one halls device is supported\n");
		return -EBUSY;
	}

	halls = devm_kzalloc(dev, sizeof(*halls), GFP_KERNEL);
	if (!halls)
		return -ENOMEM;

	halls->gpio_1 = devm_gpiod_get(dev, "hall1", GPIOD_IN);
	if (IS_ERR(halls->gpio_1)) {
		ret = PTR_ERR(halls->gpio_1);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to get hall1 gpio: %d\n", ret);
		return ret;
	}

	halls->gpio_2 = devm_gpiod_get(dev, "hall2", GPIOD_IN);
	if (IS_ERR(halls->gpio_2)) {
		ret = PTR_ERR(halls->gpio_2);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to get hall2 gpio: %d\n", ret);
		return ret;
	}

	mutex_init(&halls->lock);
	sema_init(&halls->sem_irq, 0);
	halls->hall_status = 0;
	INIT_DELAYED_WORK(&halls->notify_work, hall_notify_work_func);

	halls->hall_wakelock = wakeup_source_register(dev, "dual_hall_ws");
	if (!halls->hall_wakelock) {
		dev_err(dev, "wakeup_source_register failed!\n");
		return -ENOMEM;
	}

	halls->miscdev.fops = &halls_fops;
	halls->miscdev.minor = MISC_DYNAMIC_MINOR;
	halls->miscdev.name = DEF_HALLS_DEVICE_NAME;
	halls->miscdev.parent = dev;

	platform_set_drvdata(pdev, halls);
	g_st_halls = halls;

	ret = misc_register(&halls->miscdev);
	if (ret < 0) {
		dev_err(dev, "misc_register failed: %d\n", ret);
		g_st_halls = NULL;
		wakeup_source_unregister(halls->hall_wakelock);
		return ret;
	}

	return 0;
}

static int halls_remove(struct platform_device *pdev)
{
	struct _hall_device *halls = platform_get_drvdata(pdev);

	misc_deregister(&halls->miscdev);
	mutex_lock(&halls->lock);
	halls->in_end = 1;
	up(&halls->sem_irq);
	halls_release_irq(halls, &halls->gpio_1_irq);
	halls_release_irq(halls, &halls->gpio_2_irq);
	halls->irqs_requested = 0;
	mutex_unlock(&halls->lock);
	cancel_delayed_work_sync(&halls->notify_work);
	wakeup_source_unregister(halls->hall_wakelock);
	g_st_halls = NULL;

	return 0;
}

static const struct of_device_id halls_of_match[] = {
	{ .compatible = "xiaomi,halls" },
	{},
};
MODULE_DEVICE_TABLE(of, halls_of_match);

static struct platform_driver halls_driver = {
	.probe = halls_probe,
	.remove = halls_remove,
	.driver = {
		.name = DEF_HALLS_DEVICE_NAME,
		.of_match_table = halls_of_match,
	},
};
module_platform_driver(halls_driver);

MODULE_LICENSE("GPL v2");
