/*
 * LEDs driver for GPIOs
 *
 * Copyright (C) 2007 8D Technologies inc.
 * Raphael Assenat <raph@8d.com>
 * Copyright (C) 2008, 2014 Freescale Semiconductor, Inc.
 * Heavily modified by Erik Friesen, is only for the i.mx6 and uses the EPIT timer
 * Much code is borrowed from epit.c rather than try to integrate it. This also relies 
 * On corresponding mods to clk-imx6q.c to enable the epit timer
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */


#define HighSpeedClock 33000000
//#define LevelBits 4
//#define ShiftBits (8-LevelBits)
#define PwmHZ 100
//#define MaxPwmLevel ((1 << LevelBits)-1)
#define TimePeriod (HighSpeedClock/PwmHZ)
#define TimePeriodTick (TimePeriod/255)

#define EPITCR		0x00
#define EPITSR		0x04
#define EPITLR		0x08
#define EPITCMPR	0x0c
#define EPITCNR		0x10

#define EPITCR_EN			(1 << 0)
#define EPITCR_ENMOD			(1 << 1)
#define EPITCR_OCIEN			(1 << 2)
#define EPITCR_RLD			(1 << 3)
#define EPITCR_PRESC(x)			(((x) & 0xfff) << 4)
#define EPITCR_SWR			(1 << 16)
#define EPITCR_IOVW			(1 << 17)
#define EPITCR_DBGEN			(1 << 18)
#define EPITCR_WAITEN			(1 << 19)
#define EPITCR_RES			(1 << 20)
#define EPITCR_STOPEN			(1 << 21)
#define EPITCR_OM_DISCON		(0 << 22)
#define EPITCR_OM_TOGGLE		(1 << 22)
#define EPITCR_OM_CLEAR			(2 << 22)
#define EPITCR_OM_SET			(3 << 22)
#define EPITCR_CLKSRC_OFF		(0 << 24)
#define EPITCR_CLKSRC_PERIPHERAL	(1 << 24)
#define EPITCR_CLKSRC_REF_HIGH		(1 << 25)
#define EPITCR_CLKSRC_REF_LOW		(3 << 24)

#define EPITSR_OCIF			(1 << 0)

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/leds.h>
#include <linux/of_platform.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/module.h>
#include <linux/pinctrl/consumer.h>
#include <linux/err.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/of_irq.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include "../../../arch/arm/mach-imx/hardware.h"
#include "../../../arch/arm/mach-imx/common.h"

typedef struct {
	int enabled;
	int next;
	unsigned gpio;
	unsigned char value;
	int Inverted;
	int PinState;
	const char * Name;
}pwm_desc;


static volatile pwm_desc pwm_table[ARCH_NR_GPIOS];
static unsigned char mem_buf[ARCH_NR_GPIOS * 40];
static volatile pwm_desc * ActivePwm[ARCH_NR_GPIOS + 1] = {0};
static void __iomem *timer_base;

#ifndef CONFIG_OF_GPIO
#define CONFIG_OF_GPIO 1
#warning Remove for production
#endif

struct gpio_led_data {
	struct led_classdev cdev;
	unsigned gpio;
	struct work_struct work;
	u8 new_level;
	u8 can_sleep;
	u8 active_low;
	u8 blinking;
	u8 PwmLevel;
	int (*platform_gpio_blink_set)(unsigned gpio, int state,
			unsigned long *delay_on, unsigned long *delay_off);
};
static int DeviceOpened = 0;

static int memory_open(struct inode *i, struct file *f) {
	if (!DeviceOpened) {
		DeviceOpened = 1;
		return 0; // success
	} else {
		return -EBUSY;
	}
}

static int memory_release(struct inode *i, struct file *f) {
	DeviceOpened = 0;
	return 0; // success
}

static ssize_t memory_read(struct file *f, char *buf, size_t count, loff_t *f_pos) {

	int i, len;
	unsigned char * wbuf = mem_buf;
	if (*f_pos) {
		return 0;
	}
	i = 0;
	while (ActivePwm[i] != NULL && i < ARCH_NR_GPIOS) {
		if (ActivePwm[i]->enabled && ActivePwm[i]->Name != NULL) {
			if (ActivePwm[i]->Inverted) {
				wbuf += sprintf(wbuf, "%s=%i,", ActivePwm[i]->Name, 255 - ActivePwm[i]->value);
			} else {
				wbuf += sprintf(wbuf, "%s=%i,", ActivePwm[i]->Name, ActivePwm[i]->value);
			}
		}
		i++;
	}
	wbuf += sprintf(wbuf, "\n");
	len = wbuf - mem_buf;
	if (copy_to_user(buf, mem_buf, len)) {
		return -EINVAL;
	}
	*f_pos += len;
	return len;
}

static ssize_t memory_write(struct file *f, const char *buf, size_t count, loff_t *f_pos) {
	char * next;
	int a,found,len;
	//unsigned gpio;
	long value;
	size_t tcount = count;
	if (*f_pos) {
		return -ESPIPE;//Only allow write from start
	}
	if(count >= sizeof(mem_buf) - 1){
		return -EFBIG;
	}
	if(copy_from_user(mem_buf, buf, count)){
		return -EFAULT;
	}
	next = mem_buf;
	while (*next && tcount > 5) {
		//find name
		found = -1;
		a = 0;
		while (ActivePwm[a] != NULL && a < ARCH_NR_GPIOS) {
			if (ActivePwm[a]->enabled && ActivePwm[a]->Name != NULL) {
				len = strlen(ActivePwm[a]->Name);
				if (memcmp(next, ActivePwm[a]->Name, len) == 0) {
					found = a;
					break;
				}
			}
			a++;
		}
		if (found > -1) {
			tcount -= (len + 1);
			next += len + 1;
			if (tcount < 2) {
				break;//Stop all, should be at least "1,"
			}
			value = 0;
			for (a = 0; a < 3; a++) {
				if (tcount < 1) {
					break; //Stop all
				}
				if (*next >= '0' && *next <= '9') {
					value *= 10;
					value += *next - '0';
					next++;
					tcount--;
				} else {
					break;
				}
			}
			if (value > 255) {
				value = 255;
			} else if (value < 0) {
				value = 0;
			}
			if (ActivePwm[found]->Inverted) {
				ActivePwm[found]->value = 255 - value;
			} else {
				ActivePwm[found]->value = value;
			}
		}		
		//Search for next
		while(tcount > 5 && *next){
			tcount--;
			if(*next++ == ','){
				break;
			}			
		}
	}
	return count;
}

static struct file_operations memory_fops =
{
	.owner = THIS_MODULE,
	.read = memory_read,
	.write = memory_write,
	.open = memory_open,
	.release = memory_release	
};

static struct miscdevice gpio_pwm_miscdev = {
	11,
	"pwm-leds",
	&memory_fops,
};

static inline void epit_irq_disable(void)
{
	u32 val;

	val = __raw_readl(timer_base + EPITCR);
	val &= ~EPITCR_OCIEN;
	__raw_writel(val, timer_base + EPITCR);
}

static inline void epit_irq_enable(void)
{
	u32 val;

	val = __raw_readl(timer_base + EPITCR);
	val |= EPITCR_OCIEN;
	__raw_writel(val, timer_base + EPITCR);
}

static void epit_irq_acknowledge(void)
{
	__raw_writel(EPITSR_OCIF, timer_base + EPITSR);
}

//static unsigned int PwmLevel = 0;
static int PWM_STATE = 0;
static u32 LastTimerVal;

enum {
	PWM_START, PWM_NEXT
};
//static volatile int Lock;
static irqreturn_t epit_timer_interrupt(int irq, void *dev_id)
{
	int i, next = 255;
	u32 u, readval, diff;
	epit_irq_acknowledge();	
	switch(PWM_STATE){
		case PWM_START:
			//Set all active high, then set timer to interrupt at next
			i = 0;
			while (ActivePwm[i] != NULL && i < ARCH_NR_GPIOS) {
				if (ActivePwm[i]->enabled) {
					if (ActivePwm[i]->value) {
						if (ActivePwm[i]->value < next) {
							next = ActivePwm[i]->value;
						}
						if (!ActivePwm[i]->PinState) {
							gpio_set_value(ActivePwm[i]->gpio, 1);
							ActivePwm[i]->PinState = 1;
						}
					} else {
						if (ActivePwm[i]->PinState) {
							gpio_set_value(ActivePwm[i]->gpio, 0);
							ActivePwm[i]->PinState = 0;
						}
					}
				}
				i++;
			}
			//Calculate next interrupt, set compare register accordingly
			u = LastTimerVal - (next * TimePeriodTick);
			readval = __raw_readl(timer_base + EPITCNR);
			diff = readval - u;
			if (diff > (256 * TimePeriodTick)) {
				u = readval - TimePeriodTick; //Make sure we get an interrupt if we were delayed
			}
			__raw_writel(u, timer_base + EPITCMPR);
			if (next >= 255) {
				LastTimerVal -= TimePeriod;//Set to start of next period
			} else {
				PWM_STATE++;
			}
			break;

		case PWM_NEXT:
			readval = LastTimerVal - __raw_readl(timer_base + EPITCNR);
			readval /= TimePeriodTick;
			if (readval > 255) {
				readval = 255;
			}
			i = 0;
			while (ActivePwm[i] != NULL && i < ARCH_NR_GPIOS) {
				if (ActivePwm[i]->enabled) {
					if (ActivePwm[i]->value >= readval) {
						if (ActivePwm[i]->value < next) {
							next = ActivePwm[i]->value;
						}
					} else {
						if (ActivePwm[i]->PinState) {
							gpio_set_value(ActivePwm[i]->gpio, 0);
							ActivePwm[i]->PinState = 0;
						}
					}
				}
				i++;
			}
			//Is this the last one?
			if(next >= 255){
				LastTimerVal -= TimePeriod;//Set to start of next period
				u = LastTimerVal;
				PWM_STATE = PWM_START;
			} else {
				u = LastTimerVal - (next * TimePeriodTick);
			}
			readval = __raw_readl(timer_base + EPITCNR);
			diff = readval - u;
			if (diff > (256 * TimePeriodTick)) {
				u = readval - TimePeriodTick; //Make sure we get an interrupt if we were delayed
			}
			__raw_writel(u, timer_base + EPITCMPR);
			break;
	}
	return IRQ_HANDLED;
}

static void gpio_led_set(struct led_classdev *led_cdev,
	enum led_brightness value) {
	struct gpio_led_data *led_dat = container_of(led_cdev, struct gpio_led_data, cdev);
	int Index;
	if (value > 255) {
		value = 255;
	}
	if (value < 0) {
		value = 0;
	}
	if (led_dat->active_low) {
		led_dat->PwmLevel = 0xFF - value;
	} else {
		led_dat->PwmLevel = value;
	}
	Index = led_dat->gpio;
	if (Index < 0 || Index >= ARCH_NR_GPIOS) {
		return;
	} else {
		pwm_table[Index].value = led_dat->PwmLevel;
	}
}
/*
static int gpio_blink_set(struct led_classdev *led_cdev,
	unsigned long *delay_on, unsigned long *delay_off)
{
	printk(KERN_INFO "gpio_blink_set not supported\n");
	return 0;//Don't feel like supporting this feature, EF
}
*/

static int create_gpio_led(const struct gpio_led *template,
	struct gpio_led_data *led_dat, struct device *parent, long int brightness)//,
	//int (*blink_set)(struct gpio_desc *, int, unsigned long *, unsigned long *))
{
	int ret, state, Index, a;
	led_dat->gpio = -1;

	if(brightness > 255){
		brightness = 255;
	}
	/* skip leds that aren't available */
	if (!gpio_is_valid(template->gpio)) {
		dev_info(parent, "Skipping unavailable LED gpio %d (%s)\n",
				template->gpio, template->name);
		return 0;
	}

	ret = devm_gpio_request(parent, template->gpio, template->name);
	if (ret < 0)
		return ret;

	Index = template->gpio;
	if (Index < 0 || Index >= ARCH_NR_GPIOS) {
		//Do nothing
	} else {
		pwm_table[Index].enabled = 1;
		pwm_table[Index].PinState = 0;
		pwm_table[Index].gpio = Index;
		pwm_table[Index].value = template->active_low ? 0xFF - brightness : brightness;
		pwm_table[Index].Inverted = template->active_low ? 1 : 0;
		pwm_table[Index].Name = template->name;
		Index = 0;
		for (a = 0; a < ARCH_NR_GPIOS; a++) {
			if (pwm_table[a].enabled) {
				ActivePwm[Index++] = &pwm_table[a];
			}
		}
		ActivePwm[Index] = NULL;
	}
	led_dat->cdev.name = template->name;
	led_dat->cdev.default_trigger = template->default_trigger;
	led_dat->gpio = template->gpio;
	led_dat->can_sleep = gpio_cansleep(template->gpio);
	led_dat->active_low = template->active_low;
	led_dat->blinking = 0;
	/*if (blink_set) {
		led_dat->platform_gpio_blink_set = blink_set;
		led_dat->cdev.blink_set = gpio_blink_set;
	}*/
	led_dat->cdev.brightness_set = gpio_led_set;
	if (template->default_state == LEDS_GPIO_DEFSTATE_KEEP)
		state = !!gpio_get_value_cansleep(led_dat->gpio) ^ led_dat->active_low;
	else
		state = (template->default_state == LEDS_GPIO_DEFSTATE_ON);
	led_dat->cdev.brightness = brightness;//state ? LED_FULL : LED_OFF;
	if (!template->retain_state_suspended)
		led_dat->cdev.flags |= LED_CORE_SUSPENDRESUME;

	ret = gpio_direction_output(led_dat->gpio, led_dat->active_low ^ state);
	if (ret < 0)
		return ret;
	ret = led_classdev_register(parent, &led_dat->cdev);
	if (ret < 0)
		return ret;

	return 0;
}

static void delete_gpio_led(struct gpio_led_data *led)
{
	int Index;
	if (!gpio_is_valid(led->gpio))
		return;
	led_classdev_unregister(&led->cdev);
	Index = led->gpio;
	if (Index < 0 || Index >= ARCH_NR_GPIOS) {
		//Do nothing
	} else {
		pwm_table[Index].enabled = 0;
		//RebuildTable();
	}
	//cancel_work_sync(&led->work);
}

struct gpio_leds_priv {
	int num_leds;
	struct gpio_led_data leds[];
};

static inline int sizeof_gpio_leds_priv(int num_leds)
{
	return sizeof(struct gpio_leds_priv) +
		(sizeof(struct gpio_led_data) * num_leds);
}

/* Code to create from OpenFirmware platform devices */
#ifdef CONFIG_OF_GPIO
static struct gpio_leds_priv *gpio_leds_create_of(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node, *child;
	struct gpio_leds_priv *priv;
	int count, ret;

	/* count LEDs in this device, so we know how much to allocate */
	count = of_get_child_count(np);
	if (!count)
		return ERR_PTR(-ENODEV);

	for_each_child_of_node(np, child)
		if (of_get_gpio(child, 0) == -EPROBE_DEFER)
			return ERR_PTR(-EPROBE_DEFER);

	priv = devm_kzalloc(&pdev->dev, sizeof_gpio_leds_priv(count),
			GFP_KERNEL);
	if (!priv)
		return ERR_PTR(-ENOMEM);

	for_each_child_of_node(np, child) {
		struct gpio_led led = {};
		enum of_gpio_flags flags;
		const char *state;
		long int DefaultBrightness = 0;
		led.gpio = of_get_gpio_flags(child, 0, &flags);
		led.active_low = flags & OF_GPIO_ACTIVE_LOW;
		led.name = of_get_property(child, "label", NULL) ? : child->name;
		led.default_trigger =
			of_get_property(child, "linux,default-trigger", NULL);
		state = of_get_property(child, "default-state", NULL);
		if (state) {
			if (!strcmp(state, "keep"))
				led.default_state = LEDS_GPIO_DEFSTATE_KEEP;
			else if (!strcmp(state, "on"))
				led.default_state = LEDS_GPIO_DEFSTATE_ON;
			else
				led.default_state = LEDS_GPIO_DEFSTATE_OFF;
		}
		if(led.default_state == LEDS_GPIO_DEFSTATE_ON){
			DefaultBrightness = 255;
		}
		if (of_get_property(child, "retain-state-suspended", NULL))
			led.retain_state_suspended = 1;
		state = of_get_property(child, "default-brightness", NULL);
		if(state){
			ret = kstrtol(state, 10, &DefaultBrightness);
			if(ret){
				DefaultBrightness = 0;
			}
		}
		ret = create_gpio_led(&led, &priv->leds[priv->num_leds++], &pdev->dev, DefaultBrightness);
		if (ret < 0) {
			of_node_put(child);
			goto err;
		}
	}

	return priv;

err:
	for (count = priv->num_leds - 2; count >= 0; count--)
		delete_gpio_led(&priv->leds[count]);
	return ERR_PTR(-ENODEV);
}

static const struct of_device_id of_gpio_leds_match[] = {
	{ .compatible = "gpio-pwm-leds", },
	{},
};
#else /* CONFIG_OF_GPIO */
static struct gpio_leds_priv *gpio_leds_create_of(struct platform_device *pdev)
{
	return ERR_PTR(-ENODEV);
}
#endif /* CONFIG_OF_GPIO */


static int gpio_led_probe(struct platform_device *pdev)
{
	struct gpio_led_platform_data *pdata = pdev->dev.platform_data;
	struct gpio_leds_priv *priv;
	struct pinctrl *pinctrl;
	//struct timespec tp;
	int i, irq, ret = 0;
	struct device_node *np;
	void __iomem *base;
	//u32 readval;
	pinctrl = devm_pinctrl_get_select_default(&pdev->dev);
	if (IS_ERR(pinctrl))
		dev_warn(&pdev->dev, "pins are not configured from the driver\n");

	if (pdata && pdata->num_leds) {
		priv = devm_kzalloc(&pdev->dev,
				sizeof_gpio_leds_priv(pdata->num_leds),
					GFP_KERNEL);
		if (!priv)
			return -ENOMEM;
	//EF notes: Its unclear to me why this is in here, as it isn't used
		priv->num_leds = pdata->num_leds;
		for (i = 0; i < priv->num_leds; i++) {
			//dev_info(&pdev->dev, "gpio_led_probe = %i" , i); 
			ret = create_gpio_led(&pdata->leds[i], &priv->leds[i], &pdev->dev, 0);//pdata->gpio_blink_set);
			if (ret < 0) {
				/* On failure: unwind the led creations */
				for (i = i - 1; i >= 0; i--)
					delete_gpio_led(&priv->leds[i]);
				return ret;
			}
		}
	} else {
		priv = gpio_leds_create_of(pdev);
		if (IS_ERR(priv))
			return PTR_ERR(priv);
	}
	
	platform_set_drvdata(pdev, priv);

	np = of_find_compatible_node(NULL, NULL, "fsl,imx6q-epit");
	if (np == NULL) {
		dev_warn(&pdev->dev, "of_find_compatible_node\n");
		goto SKIP;
	}	
	if (np == NULL) {
		dev_warn(&pdev->dev, "of_iomap\n");
		goto SKIP;
	}
	
	base = of_iomap(np, 0);
	WARN_ON(!base);
	timer_base = base;

	irq = irq_of_parse_and_map(np, 0);
	if (request_irq(irq, epit_timer_interrupt, 0, "Firm PWM", 0)) {
		printk(KERN_ERR "Firm PWM: cannot register IRQ %d\n", irq);
	} else {
		dev_info(&pdev->dev, "epit_timer_init irq %i @ base %x\n", irq, (unsigned int)timer_base);
	}
	__raw_writel(0x0, timer_base + EPITCR); //Clear to all off 
	epit_irq_acknowledge(); //Clear ack bit
	LastTimerVal = 0xffffffff - HighSpeedClock; //Give one second before first
	__raw_writel(LastTimerVal, timer_base + EPITCMPR);
	__raw_writel(EPITCR_CLKSRC_PERIPHERAL | EPITCR_OCIEN | EPITCR_WAITEN | EPITCR_PRESC(1), timer_base + EPITCR);
	epit_irq_enable();
	__raw_writel(EPITCR_EN | EPITCR_CLKSRC_PERIPHERAL | EPITCR_OCIEN | EPITCR_WAITEN | EPITCR_PRESC(1), timer_base + EPITCR);
	LastTimerVal = 0;
	//Done setup
	SKIP:

	return 0;
}

static int gpio_led_remove(struct platform_device *pdev)
{
	struct gpio_leds_priv *priv = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < priv->num_leds; i++)
		delete_gpio_led(&priv->leds[i]);

	platform_set_drvdata(pdev, NULL);

	return 0;
}

static struct platform_driver gpio_led_driver = {
	.probe		= gpio_led_probe,
	.remove		= gpio_led_remove,
	.driver		= {
		.name	= "leds-gpio-pwm",
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(of_gpio_leds_match),
	},
};

static int __init gpio_pwm_init(void) {
	int rc;
	rc = misc_register(&gpio_pwm_miscdev);

	if (rc != 0) {
		printk(KERN_INFO "Failed to Global PWM Control\n");
	} else {
		printk(KERN_INFO "registered Global PWM Control\n");
	}
	return rc;
}
module_init(gpio_pwm_init);
module_platform_driver(gpio_led_driver);

MODULE_AUTHOR("Erik Friesen <friesendrywall@gmail.com>");
MODULE_DESCRIPTION("GPIO PWM LED driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:leds-gpio-pwm");
