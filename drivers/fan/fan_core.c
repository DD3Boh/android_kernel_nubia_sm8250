#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/thermal.h>
#include <linux/fb.h>
#include <linux/notifier.h>
#include <linux/msm_drm_notify.h>

#include "nubia_fan.h"
#define FAN_PINCTRL_STATE_ACTIVE "pull_up_default"
#define FAN_PINCTRL_STATE_SUSPEND "pull_down_default"
#define FAN_VREG_L6 "pm8150a_l6"
#define FAN_VREG_L6_VOLTAGE 3312000

#define STATUS_OK (0)
#define STATUS_READ_ERROR (-6)
#define STATUS_WRITE_ERROR (-7)
#define VALUE_PROMPT 0x3E
#define DELAY_MASTERERASE_MS (1000)
#define WRITECOMMAND 0xD0
#define MAX28200_LOADER_ERROR_NONE 0x00

static struct fan *nubia_fan;
static unsigned int fan_speed = 0;
static unsigned int fan_level = 0;
static unsigned int old_fan_level = 0;
static unsigned int screen_status = 1;
static bool fan_power_on = 0;
static bool fan_smart = false;
static bool fan_manual = false;
static unsigned int firmware_version = 5;
static unsigned char firmware_magicvalue = 0x55;
static unsigned char firmware_version_reg = 0x09;
int gpio11_test;
struct delayed_work fan_delay_work;

static bool get_fan_power_on_state(void)
{
	pr_err("%s: fan_power_on=%d\n", __func__, fan_power_on);
	return fan_power_on;
}

static void set_fan_power_on_state(bool state)
{
	fan_power_on = state;
	pr_err("%s: state=%d\n", __func__, state);
}

void lowlevel_delay(int microseconds)
{
	// perform platform specific delay here
	mdelay(microseconds);
}

int lowlevel_i2cWrite(uint8_t *val, int length)
{
	int ret;
	struct i2c_client *i2c = nubia_fan->i2c;
	struct i2c_msg msg[1];
	msg[0].addr = i2c->addr;
	msg[0].flags = 0;
	msg[0].len = length;
	msg[0].buf = val;

	ret = i2c_transfer(i2c->adapter, msg, 1);
	if (ret)
		return STATUS_WRITE_ERROR;
	return STATUS_OK;
}

int lowlevel_i2cRead(uint8_t *val, int length)
{
	int ret;
	struct i2c_client *i2c = nubia_fan->i2c;
	struct i2c_msg msg[1];

	msg[0].addr = i2c->addr;
	msg[0].flags = I2C_M_RD;
	msg[0].len = length;
	msg[0].buf = val;
	ret = i2c_transfer(i2c->adapter, msg, 1);
	if (ret)
		return STATUS_READ_ERROR;
	return STATUS_OK;
}

static unsigned char fan_i2c_firmware_read(struct fan *fan, unsigned char *data,
					   unsigned int length)
{
	struct i2c_client *i2c = fan->i2c;
	unsigned int ret;
	char read_data[1] = { 0 };
	struct i2c_msg msg[1];
	msg[0].addr = i2c->addr;
	msg[0].flags = I2C_M_RD;
	msg[0].len = sizeof(read_data);
	msg[0].buf = read_data;
	ret = i2c_transfer(i2c->adapter, msg, 1);
	return read_data[0];
}

int MAX28200_program(uint8_t *image, int size)
{
	int i;
	uint8_t val;
	int status;
	int index;
	int address;
	int payloadLength;
	// send the Magic Value
	val = firmware_magicvalue;
	fan_i2c_write(nubia_fan, &val, 1);

	udelay(75);
	// read the prompt
	status = fan_i2c_firmware_read(nubia_fan, &firmware_magicvalue, 1);
	if (status != VALUE_PROMPT) {
		pr_err("%s<-->%d STATUS_INVALID_PROMPT_ERROR status:%x\n",
			__func__, __LINE__, status);
		return -1;
	}

	val = 0x00;
	fan_i2c_write(nubia_fan, &val, 1);
	status = fan_i2c_firmware_read(nubia_fan, &firmware_magicvalue, 1);
	if (status != VALUE_PROMPT) {
		pr_err("%s<-->%d STATUS_INVALID_PROMPT_ERROR status:%x\n",
			__func__, __LINE__, status);
		return -1;
	}

	// Master Erase

	val = 0x02;
	fan_i2c_write(nubia_fan, &val, 1);

	// delay after issuing master erase
	lowlevel_delay(DELAY_MASTERERASE_MS);

	// S 55 [3E*] P
	// read the prompt
	status = fan_i2c_firmware_read(nubia_fan, &firmware_magicvalue, 1);
	if (status != VALUE_PROMPT) {
		pr_err("%s<-->%d STATUS_INVALID_PROMPT_ERROR status:%x\n",
			__func__, __LINE__, status);
		return -1;
	}

	// Get Status
	val = 0x04;
	fan_i2c_write(nubia_fan, &val, 1);

	// S 55 [04*] P
	status = fan_i2c_firmware_read(nubia_fan, &firmware_magicvalue, 1);
	if (status != 0x04) {
		pr_err("%s<-->%d STATUS_INVALID_PROMPT_ERROR status:%x\n",
				__func__, __LINE__, status);
		return -1;
	}

	// S 55 [00*] P
	status = fan_i2c_firmware_read(nubia_fan, &firmware_magicvalue, 1);
	if (status != 0x00) {
		pr_err("%s<-->%d STATUS_INVALID_PROMPT_ERROR status:%x\n",
			__func__, __LINE__, status);
		return -1;
	}

	// S 55 [3E*] P
	// read the prompt
	status = fan_i2c_firmware_read(nubia_fan, &firmware_magicvalue, 1);
	if (status != VALUE_PROMPT) {
		pr_err("%s<-->%d STATUS_INVALID_PROMPT_ERROR status:%x\n",
			__func__, __LINE__, status);
		return -1;
	}

	// Bogus Command
	// S 54 55 P
	val = 0x55;
	fan_i2c_write(nubia_fan, &val, 1);

	// S 54 00 P
	val = 0x00;
	fan_i2c_write(nubia_fan, &val, 1);

	// S 54 00 P
	val = 0x00;
	fan_i2c_write(nubia_fan, &val, 1);

	// S 54 00 P
	val = 0x00;
	fan_i2c_write(nubia_fan, &val, 1);

	// S 55 [3E*] P
	status = fan_i2c_firmware_read(nubia_fan, &firmware_magicvalue, 1);
	if (status != VALUE_PROMPT) {
		pr_err("%s<-->%d STATUS_INVALID_PROMPT_ERROR status:%x\n",
			__func__, __LINE__, status);
		return -1;
	}

	// Get Status
	// S 54 04 P
	val = 0x04;
	fan_i2c_write(nubia_fan, &val, 1);

	// S 55 [04*] P
	status = fan_i2c_firmware_read(nubia_fan, &firmware_magicvalue, 1);
	pr_err("%s<-->%d S55 [04*] P val:%x\n", __func__, __LINE__,
	       status);
	if (status != 0x04) {
		pr_err("%s<-->%d STATUS_INVALID_PROMPT_ERROR status:%x\n",
			__func__, __LINE__, status);
		return -1;
	}

	//S 55 [01*] P
	status = fan_i2c_firmware_read(nubia_fan, &firmware_magicvalue, 1);
	if (status != 0x01) {
		pr_err("%s<-->%d STATUS_INVALID_PROMPT_ERROR status:%x\n",
			__func__, __LINE__, status);
		return -1;
	}

	// S 55 [3E*] P
	pr_err("%s<-->%d\n", __func__, __LINE__);
	status = fan_i2c_firmware_read(nubia_fan, &firmware_magicvalue, 1);
	if (status != VALUE_PROMPT) {
		pr_err("%s<-->%d STATUS_INVALID_PROMPT_ERROR status:%x\n",
			__func__, __LINE__, status);
		return -1;
	}

	/* Set Multiplier - This value needs to be set such that bytes written multiplied by 4 gives 
	the actual desired length in the length byte for load command. */
	// S 54 0B P
	val = 0x0B;
	fan_i2c_write(nubia_fan, &val, 1);
	// S 54 00 P
	val = 0x00;
	fan_i2c_write(nubia_fan, &val, 1);
	// S 54 04 P
	val = 0x04;
	fan_i2c_write(nubia_fan, &val, 1);
	// S 54 00 P
	val = 0x00;
	fan_i2c_write(nubia_fan, &val, 1);

	// S 55 [3E*] P
	// read the prompt
	status = fan_i2c_firmware_read(nubia_fan, &firmware_magicvalue, 1);
	if (status != VALUE_PROMPT) {
		pr_err("%s<-->%d STATUS_INVALID_PROMPT_ERROR status:%x\n",
			__func__, __LINE__, status);
		return -1;
	}

	// load the image
	index = 0;
	address = 0;
	payloadLength = 16;
	while (index < size) {
		// Write Command
		val = WRITECOMMAND;
		fan_i2c_write(nubia_fan, &val, 1);

		// Byte Count
		val = (uint8_t)payloadLength;
		fan_i2c_write(nubia_fan, &val, 1);

		// Low Address
		val = (uint8_t)(address & 0xff);
		fan_i2c_write(nubia_fan, &val, 1);

		// High Address
		val = (uint8_t)((address >> 8) & 0xff);
		fan_i2c_write(nubia_fan, &val, 1);

		for (i = 0; i < payloadLength; i++) {
			val = image[index++];
			fan_i2c_write(nubia_fan, &val, 1);
		}

		// advance the address
		address += payloadLength;

		// S 55 [3E*] P
		status = fan_i2c_firmware_read(nubia_fan, &firmware_magicvalue,
					       1);
		if (status != VALUE_PROMPT) {
			pr_err("%s<-->%d STATUS_INVALID_PROMPT_ERROR status:%x\n",
				__func__, __LINE__, status);
			return -1;
		}

		// Get Status
		// S 54 04 P
		val = 0x04;
		fan_i2c_write(nubia_fan, &val, 1);

		// S 55 [04*] P
		status = fan_i2c_firmware_read(nubia_fan, &firmware_magicvalue,
					       1);
		if (status != 0x04) {
			pr_err("%s<-->%d STATUS_INVALID_PROMPT_ERROR status:%x\n",
				__func__, __LINE__, status);
			return -1;
		}

		// S 55 [00*] P
		status = fan_i2c_firmware_read(nubia_fan, &firmware_magicvalue,
					       1);
		if (status != MAX28200_LOADER_ERROR_NONE) {
			pr_err("%s<-->%d STATUS_INVALID_PROMPT_ERROR status:%x\n",
				__func__, __LINE__, status);
			return -1;
		}

		// S 55 [3E*] P
		status = fan_i2c_firmware_read(nubia_fan, &firmware_magicvalue,
					       1);
		if (status != VALUE_PROMPT) {
			pr_err("%s<-->%d STATUS_INVALID_PROMPT_ERROR status:%x\n",
				__func__, __LINE__, status);
			return -1;
		}
	}
	return STATUS_OK;
}

int MAX28200_fw_updata(void)
{
	int status;

	status = MAX28200_program(fw_002, ARRAY_SIZE(fw_002));
	if (status != STATUS_OK) {
		pr_err("Error: Programming returned with error: %d\n",
			status);
	}
	return status;
}

static void fan_i2c_write(struct fan *fan, unsigned char *data,
			  unsigned int length)
{
	int ret;
	struct i2c_client *i2c = fan->i2c;
	struct i2c_msg msg[1];
	msg[0].addr = i2c->addr;
	msg[0].flags = 0;
	msg[0].len = length;
	msg[0].buf = data;

	ret = i2c_transfer(i2c->adapter, msg, 1);
}

static unsigned int fan_i2c_read(struct fan *fan, unsigned char *data,
				 unsigned int length)
{
	struct i2c_client *i2c = fan->i2c;
	unsigned int ret1, ret2;
	unsigned int ret;
	char read_data[2] = { 0, 0 };
	struct i2c_msg msg[2];
	msg[0].addr = i2c->addr;
	msg[0].flags = 0;
	msg[0].len = length;
	msg[0].buf = data;

	msg[1].addr = i2c->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].len = sizeof(read_data);
	msg[1].buf = read_data;

	ret = i2c_transfer(i2c->adapter, msg, 2);
	ret1 = read_data[0];
	ret2 = read_data[1];
	ret = ret1 + ret2 * 256;

	return ret;
}

static void start_speed_count(struct fan *fan)
{
	static unsigned char data = 0x01;

	fan_i2c_write(fan, &data, sizeof(data));
	pr_err("%s:fan_speed=%d\n", __func__, fan_speed);
}

static void stop_speed_count(struct fan *fan)
{
	static unsigned char data = 0x03;

	fan_i2c_write(fan, &data, sizeof(data));
	pr_err("%s:fan_speed=%d\n", __func__, fan_speed);
}

static unsigned int get_speed_count(struct fan *fan)
{
	static unsigned char data = 0x02;

	fan_speed = fan_i2c_read(fan, &data, sizeof(data));
	fan_speed = fan_speed * 20; // the minute speed
	pr_err("%s:fan_speed=%d,fan_level=%d\n", __func__, fan_speed,
		fan_level);
	return fan_speed;
}

static void start_pwm(struct fan *fan, unsigned short pwm_value)
{
	static unsigned char data[2] = { 0x08, 0x00 }; // 25khz reg

	data[1] = pwm_value;
	fan_i2c_write(fan, data, sizeof(data));
}

static void get_fan_read(void)
{
	unsigned int fan_count;

	if ((get_fan_power_on_state() == true)) {
		start_speed_count(nubia_fan);
		mdelay(1000);
		stop_speed_count(nubia_fan);

		fan_count = get_speed_count(nubia_fan) / 3;

		/* If fan is running and read fan_count is 0, reset the fan */
		if ((fan_speed == 0) && get_fan_power_on_state()) {
			pr_err("%s:begin reset the fan!!!,fan_level=%d\n",
				__func__, fan_level);
			set_fan_power_on_state(false);
			fan_set_pwm_by_level(fan_level);
		}

	} else
		fan_speed = 0;

	pr_err("%s:fan_count=%d\n", __func__, fan_count);
}

static void fan_read_workqueue(struct work_struct *work)
{
	cancel_delayed_work(&fan_delay_work);
	get_fan_read();
}

static int fan_enable_reg(struct fan *fan, bool enable)
{
	int ret;

	pr_err("%s: enable=%d\n", __func__, enable);

	if (!enable) {
		ret = 0;
		goto disable_pwr_reg;
	}

	if ((fan->pwr_reg) && (regulator_is_enabled(fan->pwr_reg) == 0)) {
		ret = regulator_enable(fan->pwr_reg);
		if (ret < 0) {
			dev_err(fan->dev->parent,
				"%s: Failed to enable power regulator\n",
				__func__);
		}
	}

	gpio_free(gpio11_test);
	ret = gpio_request(gpio11_test, "GPIO11");
	if (ret) {
		pr_err("%s: fan reset gpio request failed\n", __func__);
		return ret;
	}
	gpio_direction_output(gpio11_test, 0);
	msleep(630);
	gpio_direction_output(gpio11_test, 1);
	mdelay(100);
	return ret;

disable_pwr_reg:
	gpio_direction_output(gpio11_test, 0);
	gpio_free(gpio11_test);

	if (fan->pwr_reg)
		regulator_disable(fan->pwr_reg);

	return ret;
}

static int fan_hw_reset(struct fan *fan, unsigned int delay)
{
	int ret;
	unsigned int reset_delay_time = 0;

	pr_info("%s enter %d\n", __func__, delay);
	reset_delay_time = delay;

	gpio_free(gpio11_test);
	ret = gpio_request(gpio11_test, "GPIO11");
	if (ret) {
		pr_err("%s: fan reset gpio request failed\n", __func__);
		return ret;
	}
	gpio_direction_output(gpio11_test, 1);
	msleep(10);
	gpio_direction_output(gpio11_test, 0);
	msleep(100);
	gpio_direction_output(gpio11_test, 1);

	switch (reset_delay_time) {
		case DELAY_900:
			udelay(900);
			break;
		case DELAY_1000:
			udelay(500);
			udelay(500);
			break;
		case DELAY_1100:
			udelay(500);
			udelay(600);
			break;
		case DELAY_1200:
			udelay(600);
			udelay(600);
			break;
		case DELAY_800:
			udelay(800);
			break;
		case DELAY_600:
			udelay(600);
			break;
		case DELAY_400:
			udelay(400);
			break;
		case DELAY_1300:
			udelay(600);
			udelay(700);
			break;
		case DELAY_1400:
			udelay(700);
			udelay(700);
			break;
		case DELAY_1500:
			udelay(800);
			udelay(700);
			break;
		case DELAY_200:
			udelay(200);
			break;
		case DELAY_1800:
			udelay(900);
			udelay(900);
			break;
		default:
			udelay(900);
			break;
	}

	return 0;
}

static void fan_set_enable(bool enable)
{
	pr_err("%s: enable=%d\n", __func__, enable);

	if (!enable) {
		start_pwm(nubia_fan, 0);
		fan_speed = 0;
		fan_level = 0;
	}

	set_fan_power_on_state(enable);
	fan_enable_reg(nubia_fan, enable);
}

static void fan_set_pwm_by_level(unsigned int level)
{
	static unsigned int old_level = 0;

	pr_err("%s: level=%d,old_level=%d,fan_speed=%d\n", __func__,
	       level, old_level, fan_speed);

	if (level < FAN_LEVEL_0 || level > FAN_LEVEL_MAX)
		return;

	if (level == FAN_LEVEL_0) {
		fan_set_enable(false);
		old_level = level;
	} else if (screen_status) {
		if (get_fan_power_on_state() == false) {
			fan_set_enable(true);
			old_level = 0;
		}

		if (old_level != level) {
			switch (level) {
			case FAN_LEVEL_1:
				start_pwm(nubia_fan, 20);
				break;
			case FAN_LEVEL_2:
				start_pwm(nubia_fan, 40);
				break;
			case FAN_LEVEL_3:
				start_pwm(nubia_fan, 50);
				break;
			case FAN_LEVEL_4:
				start_pwm(nubia_fan, 80);
				break;
			case FAN_LEVEL_5:
				start_pwm(nubia_fan, 100);
				break;
			default:
				break;
			}
		}
		old_level = level;
		schedule_delayed_work(&fan_delay_work, round_jiffies_relative(msecs_to_jiffies(200)));
	}
	fan_level = level;
}

static void smart_fan_func(struct work_struct *work) {
	struct fan *fan = container_of(to_delayed_work(work), typeof(*fan), smart_fan_work);
	static int level = 0, temp = 0;

	if (fan_smart && screen_status) {
		thermal_zone_get_temp(thermal_zone_get_zone_by_name("gpu-skin-avg-step"), &temp);

		if (temp > 50000)
			level = FAN_LEVEL_5;
		else if (temp > 47000)
			level = FAN_LEVEL_4;
		else if (temp > 43000)
			level = FAN_LEVEL_3;
		else if (temp > 39000)
			level = FAN_LEVEL_2;
		else if (temp > 35000)
			level = FAN_LEVEL_1;
		else
			level = FAN_LEVEL_0;

		if (level != fan_level)
			fan_set_pwm_by_level(level);
	} else if (fan_smart && !screen_status)
		old_fan_level = fan_level;
	else if (!fan_smart && !fan_manual && fan_level != 0)
		fan_set_pwm_by_level(FAN_LEVEL_0);

	queue_delayed_work(fan->wq, &fan->smart_fan_work, msecs_to_jiffies(10000));
}

static void fan_set_old_level(struct work_struct *work) {
	fan_set_pwm_by_level(old_fan_level);
};

static int fb_notifier_callback(struct notifier_block *self,
			unsigned long event, void *data) {
	struct fan *fan = container_of(self, struct fan, fb_notif);
	struct fb_event *evdata = data;
	int *blank;

	if (evdata && evdata->data && fan) {
		blank = evdata->data;
		if (*blank == MSM_DRM_BLANK_POWERDOWN) {
			screen_status = 0;
			old_fan_level = fan_level;
			if (fan_level != FAN_LEVEL_0)
				fan_set_pwm_by_level(FAN_LEVEL_0);
		} else {
			screen_status = 1;
			if (old_fan_level != FAN_LEVEL_0)
				schedule_delayed_work(&fan->pwm_delayed_work, msecs_to_jiffies(10));
		}
	}

	pr_err("Screen status: %d\n", screen_status);
	return 0;
}

static ssize_t fan_smart_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf) {
	return sprintf(buf, "%d\n", fan_smart);
}

static ssize_t fan_smart_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int input;

	if (sscanf(buf, "%d", &input) != 1)
		return -EINVAL;

	fan_smart = input > 0 ? true : false;

	if (fan_smart) {
		fan_manual = false;
		fan_set_pwm_by_level(FAN_LEVEL_0);
	}

	return count;
};

static ssize_t fan_speed_level_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", fan_level);
}

static ssize_t fan_speed_level_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	unsigned int level;
	static unsigned int old_level = 0;

	sscanf(buf, "%d", &level);

	pr_err("%s: level=%d,old_level=%d\n", __func__, level,
	       old_level);
	if (level == old_level && level == 0)
		pr_err("%s: off before\n", __func__);
	else {
		old_level = level;
		fan_set_pwm_by_level(level);
	}

	fan_manual = level > 0 ? true : false;

	if (fan_manual)
		fan_smart = false;

	return count;
}

static ssize_t fan_speed_count_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	if (get_fan_power_on_state() && fan_level != 0) {
		fan_set_pwm_by_level(fan_level);
	}

	pr_err("%s: fan_speed=%d\n", __func__, fan_speed);
	return sprintf(buf, "%d\n", fan_speed);
}

static struct kobj_attribute fan_smart_attr=
	__ATTR(fan_smart, 0664, fan_smart_show, fan_smart_store);
static struct kobj_attribute fan_level_attr = __ATTR(
	fan_speed_level, 0664, fan_speed_level_show, fan_speed_level_store);
static struct kobj_attribute fan_speed_attr =
	__ATTR(fan_speed_count, 0664, fan_speed_count_show, NULL);

static struct attribute *fan_attrs[] = {
	&fan_smart_attr.attr,
	&fan_level_attr.attr,
	&fan_speed_attr.attr,
	NULL,
};

static struct attribute_group fan_attr_group = {
	.attrs = fan_attrs,
};
struct kobject *fan_kobj;

static int fan_probe(struct i2c_client *i2c, const struct i2c_device_id *id)
{
	struct fan *fan;
	struct device_node *np = i2c->dev.of_node;
	int ret;
	unsigned int delay_table[] = { DELAY_900,  DELAY_1000, DELAY_1100,
					DELAY_1200, DELAY_800,  DELAY_600,
					DELAY_400,  DELAY_1300, DELAY_1400,
					DELAY_1500, DELAY_1800, DELAY_200 };
	unsigned int try_count = 0;
	unsigned int i = 0;

	pr_err("fan_probe enter\n");

	fan = devm_kzalloc(&i2c->dev, sizeof(struct fan), GFP_KERNEL);
	if (fan == NULL)
		return -ENOMEM;

	fan->dev = &i2c->dev;
	fan->i2c = i2c;

	fan->pwr_reg = regulator_get(fan->dev->parent, FAN_VREG_L6);
	if (IS_ERR(fan->pwr_reg)) {
		dev_err(fan->dev->parent, "%s: Failed to get power regulator\n",
			__func__);
		ret = PTR_ERR(fan->pwr_reg);
		goto regulator_put;
	}

	ret = regulator_set_voltage(fan->pwr_reg, FAN_VREG_L6_VOLTAGE,
				    FAN_VREG_L6_VOLTAGE);
	if (ret) {
		dev_err(fan->dev->parent,
			"Regulator vdd_fan set vtg failed rc=%d\n", ret);
		goto regulator_put;
	}
	if (fan->pwr_reg) {
		ret = regulator_enable(fan->pwr_reg);
		if (ret < 0) {
			dev_err(fan->dev->parent,
				"%s: Failed to enable power regulator\n",
				__func__);
		}
	}

	nubia_fan = fan;
	gpio11_test = of_get_named_gpio(np, "fan,reset-gpio", 0);

	fan_hw_reset(fan, 900);
	mdelay(100); //delay 0.1 seconds
	ret = fan_i2c_read(fan, &firmware_version_reg,
			   sizeof(firmware_version_reg));
	pr_err("%s: ret=%d\n", __func__, ret);

	if (ret < firmware_version) {
		try_count = ARRAY_SIZE(delay_table);

		for (i = 0; i < try_count; i++) {
			fan_hw_reset(fan, delay_table[i]);

			if (MAX28200_fw_updata() < 0) {
				pr_err("fan_fw_updata failed %d\n",
				       delay_table[i]);
				continue;
			} else {
				pr_err("fan_fw_updata done\n");
				break;
			}
		}

		mdelay(10);
		fan_hw_reset(fan, 900);
		mdelay(100); //delay 0.1 seconds
	}

	fan_kobj = kobject_create_and_add("fan", kernel_kobj);
	if (!fan_kobj) {
		pr_err("%s: fan kobj create error\n", __func__);
		return -ENOMEM;
	}
	ret = sysfs_create_group(fan_kobj, &fan_attr_group);
	if (ret) {
		pr_err("%s: failed to create fan group attributes\n",
		       __func__);
	}

	//Begin [0016004715,fix the factory test result to panic,20181121]
	INIT_DELAYED_WORK(&fan_delay_work, fan_read_workqueue);
	//End [0016004715,fix the factory test result to panic,20181121]
	fan_set_enable(false);

	/* initialize smart fan wq */
	fan->wq = alloc_workqueue("smart_fan",
				WQ_HIGHPRI | WQ_UNBOUND, 0);

	INIT_DELAYED_WORK(&fan->smart_fan_work, smart_fan_func);
	queue_delayed_work(fan->wq, &fan->smart_fan_work, msecs_to_jiffies(20000));

	INIT_DELAYED_WORK(&fan->pwm_delayed_work, fan_set_old_level);

	/* start fb notifier */
	fan->fb_notif.notifier_call = fb_notifier_callback;
	msm_drm_panel_register_client(&fan->fb_notif);

	return 0;

regulator_put:
	if (fan->pwr_reg) {
		regulator_put(fan->pwr_reg);
		fan->pwr_reg = NULL;
	}
	devm_kfree(&i2c->dev, fan);
	fan = NULL;
	return ret;
}

static int fan_remove(struct i2c_client *i2c)
{
	struct fan *fan = i2c_get_clientdata(i2c);

	pr_info("%s remove\n", __func__);

	sysfs_remove_group(fan_kobj, &fan_attr_group);
	fan_kobj = NULL;

	if (gpio_is_valid(fan->reset_gpio))
		devm_gpio_free(&i2c->dev, fan->reset_gpio);

	fan_enable_reg(fan, false);

	if (fan->pwr_reg) {
		regulator_put(fan->pwr_reg);
		fan->pwr_reg = NULL;
	}

	devm_kfree(&i2c->dev, fan);
	fan = NULL;

	msm_drm_panel_unregister_client(&fan->fb_notif);
	return 0;
}

static const struct i2c_device_id fan_i2c_id[] = {};

static const struct of_device_id of_match[] = { 
	{ .compatible = "nubia_fan_i2c" },
	{}
};

static struct i2c_driver fan_i2c_driver = {
	.driver = {
		.name = "nubia_fan",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(of_match),
	},
	.probe = fan_probe,
	.remove = fan_remove,
	.id_table = fan_i2c_id,
};

static int __init fan_init(void)
{
	int ret = 0;
	ret = i2c_add_driver(&fan_i2c_driver);

	if (ret) {
		pr_err("fail to add fan device into i2c\n");
		return ret;
	}

	return 0;
}

static void __exit fan_exit(void)
{
	i2c_del_driver(&fan_i2c_driver);
}

module_init(fan_init);
module_exit(fan_exit);

MODULE_AUTHOR("Fan, Inc.");
MODULE_DESCRIPTION("Fan Driver");
MODULE_LICENSE("GPL v2");
