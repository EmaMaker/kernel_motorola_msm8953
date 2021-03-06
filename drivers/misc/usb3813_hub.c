/*
 * Copyright (C) 2015 Motorola Mobility LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program;
 *
 */
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/param.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/clk.h>
#include <linux/regulator/consumer.h>
#include <video/slimport_device.h>
#include <linux/mods/usb3813.h>

#define USB_ATTACH 0xAA55
#define CFG_ACCESS 0x9937
#define HS_P2_BOOST 0x68CA

#define HS_BOOST_MAX 0x07

static unsigned int boost_val = HS_BOOST_MAX;
module_param(boost_val, uint, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(boost_val, "Boost Value for the USB3813 hub");

struct usb3813_info {
	struct i2c_client *client;
	struct device *dev;
	struct gpio hub_reset_n;
	struct clk *hub_clk;
	bool   hub_enabled;
	struct delayed_work usb3813_attach_work;
	struct mutex	i2c_mutex;
	struct dentry *debug_root;
	u16 debug_address;
	bool debug_enabled;
	bool debug_attach;
	struct regulator *vdd_hsic;
	bool hsic_enabled;
};

static int usb3813_write_command(struct usb3813_info *info, u16 command)
{
	struct i2c_client *client = to_i2c_client(info->dev);
	struct i2c_msg msg[1];
	u8 data[3];
	int ret;

	data[0] = (command >> 8) & 0xFF;
	data[1] = command & 0xFF;
	data[2] = 0x00;

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].buf = data;
	msg[0].len = ARRAY_SIZE(data);

	mutex_lock(&info->i2c_mutex);
	ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
	mutex_unlock(&info->i2c_mutex);

	/* i2c_transfer returns number of messages transferred */
	if (ret < 0)
		return ret;
	else if (ret != 1)
		return -EIO;

	return 0;
}

static int usb3813_write_cfg_reg(struct usb3813_info *info, u16 reg, u8 val)
{
	struct i2c_client *client = to_i2c_client(info->dev);
	struct i2c_msg msg[1];
	u8 data[8];
	int ret;

	data[0] = 0x00;
	data[1] = 0x00;
	data[2] = 0x05;
	data[3] = 0x00;
	data[4] = 0x01;
	data[5] = (reg >> 8) & 0xFF;
	data[6] = reg & 0xFF;
	data[7] = val;

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].buf = data;
	msg[0].len = ARRAY_SIZE(data);

	mutex_lock(&info->i2c_mutex);
	ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
	mutex_unlock(&info->i2c_mutex);

	if (ret < 0)
		return ret;
	else if (ret != 1)
		return -EIO;

	return usb3813_write_command(info, CFG_ACCESS);
}

static int usb3813_read_cfg_reg(struct usb3813_info *info, u16 reg)
{
	struct i2c_client *client = to_i2c_client(info->dev);
	struct i2c_msg msg[2];
	u8 data[7];
	u8 val[2];
	int ret;

	data[0] = 0x00;
	data[1] = 0x00;
	data[2] = 0x04;
	data[3] = 0x01;
	data[4] = 0x01;
	data[5] = (reg >> 8) & 0xFF;
	data[6] = reg & 0xFF;

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].buf = data;
	msg[0].len = ARRAY_SIZE(data);

	mutex_lock(&info->i2c_mutex);
	ret = i2c_transfer(client->adapter, msg, 1);
	mutex_unlock(&info->i2c_mutex);

	if (ret < 0)
		return ret;
	else if (ret != 1)
		return -EIO;

	ret = usb3813_write_command(info, CFG_ACCESS);
	if (ret < 0)
		return ret;

	data[0] = 0x00;
	data[1] = 0x04;
	msg[0].len = 2;
	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].buf = val;
	msg[1].len = ARRAY_SIZE(val);

	mutex_lock(&info->i2c_mutex);
	ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
	mutex_unlock(&info->i2c_mutex);

	if (ret < 0)
		return ret;

	return val[1];
}

static int set_hsic_state(struct usb3813_info *info, bool enable)
{
	int ret;

	if (enable && !info->hsic_enabled) {
		ret = regulator_enable(info->vdd_hsic);

		if (ret)
			dev_err(info->dev, "Unable to enable vdd_hsic\n");
		else
			info->hsic_enabled = true;
	} else if (!enable && info->hsic_enabled) {
		ret = regulator_disable(info->vdd_hsic);

		if (ret)
			dev_err(info->dev, "Unable to disable vdd_hsic\n");
		else
			info->hsic_enabled = false;
	}

	return ret;
}

static ssize_t usb3813_hsic_vdd_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	struct usb3813_info *info = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n", info->hsic_enabled);
}

static ssize_t usb3813_hsic_vdd_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct usb3813_info *info = dev_get_drvdata(dev);
	unsigned long r, mode;

	r = kstrtoul(buf, 0, &mode);
	if (r) {
		dev_err(dev, "Invalid value = %lu\n", mode);
		return -EINVAL;
	}

	mode = !!mode;

	if (set_hsic_state(info, mode))
		return -EFAULT;
	else
		return count;
}

static DEVICE_ATTR(hsic_vdd, 0660,
			usb3813_hsic_vdd_show, usb3813_hsic_vdd_store);

int usb3813_enable_hub(struct i2c_client *client,
			bool enable,
			enum usb_ext_path path)
{
	struct usb3813_info *info = i2c_get_clientdata(client);

	if (!info)
		return -EINVAL;

	if (enable == info->hub_enabled)
		return -EINPROGRESS;

	info->hub_enabled = enable;
	cancel_delayed_work(&info->usb3813_attach_work);

	if (info->hub_enabled) {
		if (clk_prepare_enable(info->hub_clk)) {
			dev_err(info->dev, "%s: failed to prepare clock\n",
								__func__);
			return -EFAULT;
		}
		gpio_set_value(info->hub_reset_n.gpio, 1);
		schedule_delayed_work(&info->usb3813_attach_work,
					msecs_to_jiffies(1000));

		if (path == USB_EXT_PATH_BRIDGE)
			set_hsic_state(info, true);
	} else {
		gpio_set_value(info->hub_reset_n.gpio, 0);
		clk_disable_unprepare(info->hub_clk);
		set_hsic_state(info, false);
	}

	return 0;
}
EXPORT_SYMBOL(usb3813_enable_hub);

static void usb3813_attach_w(struct work_struct *work)
{
	struct usb3813_info *info =
		container_of(work, struct usb3813_info,
		usb3813_attach_work.work);
	int ret;

	if (!info->hub_enabled)
		return;

	/* Reset the slimport since USB2 shares lines */
	slimport_reset_standby();

	ret = usb3813_write_cfg_reg(info, HS_P2_BOOST, boost_val);
	if (ret < 0)
		dev_err(info->dev, "Write HS_P2_BOOST failed (%d)\n", ret);

	ret = usb3813_write_command(info, USB_ATTACH);
	if (ret < 0) {
		dev_err(info->dev, "USB_ATTCH failed (%d)\n", ret);
	}

}

static int get_reg(void *data, u64 *val)
{
	struct usb3813_info *info = data;
	u8 temp = 0;

	if (!info->debug_enabled) {
		dev_err(info->dev, "Enable hub debug before access\n");
		return -ENODEV;
	}

	temp = usb3813_read_cfg_reg(info, info->debug_address);
	*val = temp;

	return 0;
}

static int set_reg(void *data, u64 val)
{
	struct usb3813_info *info = data;
	u8 temp;
	int ret;

	if (!info->debug_enabled) {
		dev_err(info->dev, "Enable hub debug before access\n");
		return -ENODEV;
	}

	temp = (u8) val;
	ret = usb3813_write_cfg_reg(info, info->debug_address, temp);
	if (ret < 0)
		dev_err(info->dev, "Write to 0x%04x failed (%d)\n",
						info->debug_address,
						ret);
	return ret;
}

DEFINE_SIMPLE_ATTRIBUTE(usb3813_reg_fops, get_reg, set_reg, "0x%02llx\n");

static int usb3813_dbg_enable_show(void *data, u64 *val)
{
	struct usb3813_info *info = data;

	*val = (u64)info->debug_enabled;
	return 0;
}

static int usb3813_dbg_enable_write(void *data, u64 val)
{
	struct usb3813_info *info = data;

	if (info->hub_enabled) {
		dev_err(info->dev, "Cannot enable debugging, HUB active\n");
		return -EBUSY;
	}

	if (val) {
		info->debug_enabled = true;
		if (clk_prepare_enable(info->hub_clk)) {
			dev_err(info->dev, "%s: failed to prepare clock\n",
								__func__);
			return -EFAULT;
		}
		gpio_set_value(info->hub_reset_n.gpio, 1);
		info->debug_enabled = true;
	} else {
		gpio_set_value(info->hub_reset_n.gpio, 0);
		clk_disable_unprepare(info->hub_clk);
		info->debug_enabled = false;
	}

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(usb3813_dbg_fops, usb3813_dbg_enable_show,
		usb3813_dbg_enable_write, "%llu\n");

static int usb3813_dbg_attach_write(void *data, u64 val)
{
	struct usb3813_info *info = data;
	int ret = 0;

	if (!info->debug_enabled) {
		dev_err(info->dev, "Debug not enabled\n");
		return -EINVAL;
	}

	if (val)
		ret = usb3813_write_command(info, USB_ATTACH);
	else {
		gpio_set_value(info->hub_reset_n.gpio, 0);
		mdelay(10);
		gpio_set_value(info->hub_reset_n.gpio, 1);
	}

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(usb3813_attach_fops, NULL,
		usb3813_dbg_attach_write, "%llu\n");

int usb3813_debug_init(struct usb3813_info *info)
{
	struct dentry *ent;

	info->debug_root = debugfs_create_dir("usb3813", NULL);

	if (!info->debug_root) {
		dev_err(info->dev, "Couldn't create debug dir\n");
		return -EINVAL;
	}

	ent = debugfs_create_x16("address", S_IFREG | S_IWUSR | S_IRUGO,
				info->debug_root, &(info->debug_address));
	if (!ent) {
		dev_err(info->dev, "Error creating address entry\n");
		return -EINVAL;
	}

	ent = debugfs_create_file("data", S_IFREG | S_IWUSR | S_IRUGO,
				info->debug_root, info, &usb3813_reg_fops);
	if (!ent) {
		dev_err(info->dev, "Error creating data entry\n");
		return -EINVAL;
	}

	ent = debugfs_create_file("enable_dbg", S_IFREG | S_IWUSR | S_IRUGO,
				info->debug_root, info, &usb3813_dbg_fops);
	if (!ent) {
		dev_err(info->dev, "Error creating enable_dbg entry\n");
		return -EINVAL;
	}

	ent = debugfs_create_file("attach", S_IFREG | S_IWUSR | S_IRUGO,
				info->debug_root, info, &usb3813_attach_fops);
	if (!ent) {
		dev_err(info->dev, "Error creating attach entry\n");
		return -EINVAL;
	}

	return 0;
}

static int usb3813_probe(struct i2c_client *client,
			      const struct i2c_device_id *id)
{
	int ret = 0;
	enum of_gpio_flags flags;
	struct usb3813_info *info;
	struct device_node *np = client->dev.of_node;

	if (!np) {
		dev_err(&client->dev, "No OF DT node found.\n");
		return -ENODEV;
	}

	info = devm_kzalloc(&client->dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->client = client;
	info->dev = &client->dev;
	i2c_set_clientdata(client, info);

	mutex_init(&info->i2c_mutex);
	info->hub_enabled = 0;

	info->hub_reset_n.gpio = of_get_gpio_flags(np, 0, &flags);
	info->hub_reset_n.flags = flags;
	of_property_read_string_index(np, "gpio-labels", 0,
				      &info->hub_reset_n.label);
	dev_dbg(&client->dev, "GPIO: %d  FLAGS: %ld  LABEL: %s\n",
		info->hub_reset_n.gpio, info->hub_reset_n.flags,
		info->hub_reset_n.label);

	ret = gpio_request_one(info->hub_reset_n.gpio,
			       info->hub_reset_n.flags,
			       info->hub_reset_n.label);
	if (ret) {
		dev_err(&client->dev, "failed to request GPIO\n");
		return -ENODEV;
	}

	if (info->hub_reset_n.flags & GPIOF_EXPORT) {
		ret = gpio_export_link(&client->dev,
			       info->hub_reset_n.label,
			       info->hub_reset_n.gpio);
		if (ret) {
			dev_err(&client->dev, "Failed to link GPIO %s: %d\n",
				info->hub_reset_n.label,
				info->hub_reset_n.gpio);
			goto fail_gpio;
		}
	}

	info->hub_clk = devm_clk_get(&client->dev, "hub_clk");
	if (IS_ERR(info->hub_clk)) {
		dev_err(&client->dev, "%s: failed to get clock.\n", __func__);
		ret = PTR_ERR(info->hub_clk);
		goto fail_gpio;
	}

	info->vdd_hsic = devm_regulator_get(&client->dev, "vdd-hsic");
	if (IS_ERR(info->vdd_hsic))
		dev_err(&client->dev, "unable to get hsic supply\n");

	INIT_DELAYED_WORK(&info->usb3813_attach_work, usb3813_attach_w);

	ret = device_create_file(&client->dev, &dev_attr_hsic_vdd);
	if (ret) {
		dev_err(&client->dev, "Unable to create hsic_vdd file\n");
		goto fail_clk;
	}

	usb3813_debug_init(info);

	dev_info(&client->dev, "Done probing usb3813\n");
	return 0;

fail_clk:
	clk_put(info->hub_clk);
fail_gpio:
	gpio_free(info->hub_reset_n.gpio);
	kfree(info);
	return ret;
}

static int usb3813_remove(struct i2c_client *client)
{
	struct usb3813_info *info = i2c_get_clientdata(client);

	cancel_delayed_work(&info->usb3813_attach_work);
	gpio_free(info->hub_reset_n.gpio);
	if (info->hub_enabled)
		clk_disable_unprepare(info->hub_clk);
	clk_put(info->hub_clk);
	devm_regulator_put(info->vdd_hsic);
	device_remove_file(&client->dev, &dev_attr_hsic_vdd);
	debugfs_remove_recursive(info->debug_root);
	kfree(info);
	return 0;
}

static const struct of_device_id usb3813_of_tbl[] = {
	{ .compatible = "microchip,usb3813", .data = NULL},
	{},
};

static const struct i2c_device_id usb3813_id[] = {
	{"usb3813-hub", 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, usb3813_id);

static struct i2c_driver usb3813_driver = {
	.driver		= {
		.name		= "usb3813",
		.owner		= THIS_MODULE,
		.of_match_table	= usb3813_of_tbl,
	},
	.probe		= usb3813_probe,
	.remove		= usb3813_remove,
	.id_table	= usb3813_id,
};
module_i2c_driver(usb3813_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Motorola Mobility LLC");
MODULE_DESCRIPTION("usb3813-hub driver");
MODULE_ALIAS("i2c:usb3813-hub");

