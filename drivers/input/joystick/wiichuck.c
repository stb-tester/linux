/*
 * Nintendo Nunchuck driver for i2c connection.
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/input-polldev.h>
#include <linux/mod_devicetable.h>
#include <linux/slab.h>

MODULE_DESCRIPTION("Nintendo Nunchuck driver");
MODULE_AUTHOR("Korneliusz Jarzebski <korneliusz <at> jarzebski.pl>");
MODULE_LICENSE("GPL");

#define MASK_BUTTON_Z 0x01
#define MASK_BUTTON_C 0x02
#define MASK_ACCEL_X 0x0C

#define BUTTON_Z(a) !(a & MASK_BUTTON_Z)
#define BUTTON_C(a) !((a & MASK_BUTTON_C) >> 1)
#define ACCEL_X(a, b) ((a << 2) | ((b & MASK_ACCEL_X) >> 2))

struct wiichuck_device {
	struct input_polled_dev *poll_dev;
	struct i2c_client *i2c_client;
	int state;
};

static void wiichuck_poll(struct input_polled_dev *poll_dev)
{
	struct wiichuck_device *wiichuck = poll_dev->private;
	struct i2c_client *i2c = wiichuck->i2c_client;
	static uint8_t cmd_byte = 0;
	struct i2c_msg cmd_msg = { .addr = i2c->addr, .len = 1, .buf = &cmd_byte };
	uint8_t b[6];
	struct i2c_msg data_msg = { .addr = i2c->addr, .flags = I2C_M_RD, .len = 6, .buf = b };
	int jx, jy, ax, ay, az;
	bool c, z;

	switch (wiichuck->state) {
	case 0:
		i2c_transfer(i2c->adapter, &cmd_msg, 1);
		wiichuck->state = 1;
		break;

	case 1:
		i2c_transfer(i2c->adapter, &data_msg, 1);

		jx = b[0];
		jy = b[1];

		ax = ACCEL_X(b[2], b[5]);
		ay = ACCEL_X(b[3], b[5]);
		az = ACCEL_X(b[4], b[5]);

		z = BUTTON_Z(b[5]);
		c = BUTTON_C(b[5]);

		input_report_abs(poll_dev->input, ABS_X, jx);
		input_report_abs(poll_dev->input, ABS_Y, jy);
		input_report_abs(poll_dev->input, ABS_RX, ax);
		input_report_abs(poll_dev->input, ABS_RY, ay);
		input_report_abs(poll_dev->input, ABS_RZ, az);
		input_report_key(poll_dev->input, BTN_C, c);
		input_report_key(poll_dev->input, BTN_Z, z);

		input_sync(poll_dev->input);

		wiichuck->state = 0;

		break;

	default:
		wiichuck->state = 0;
	}
}

static void wiichuck_open(struct input_polled_dev *poll_dev)
{
	struct wiichuck_device *wiichuck = poll_dev->private;
	struct i2c_client *i2c = wiichuck->i2c_client;
	static uint8_t data1[2] = { 0xf0, 0x55 };
	static uint8_t data2[2] = { 0xfb, 0x00 };
	struct i2c_msg msg1 = { .addr = i2c->addr, .len = 2, .buf = data1 };
	struct i2c_msg msg2 = { .addr = i2c->addr, .len = 2, .buf = data2 };

	i2c_transfer(i2c->adapter, &msg1, 1);
	i2c_transfer(i2c->adapter, &msg2, 1);
	wiichuck->state = 0;
}

static int wiichuck_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct wiichuck_device *wiichuck;
	struct input_polled_dev *poll_dev;
	struct input_dev *input_dev;
	int rc;

	wiichuck = kzalloc(sizeof(*wiichuck), GFP_KERNEL);
	if (!wiichuck)
		return -ENOMEM;

	poll_dev = input_allocate_polled_device();
	if (!poll_dev) {
		rc = -ENOMEM;
		goto err_alloc;
	}

	wiichuck->i2c_client = client;
	wiichuck->poll_dev = poll_dev;

	poll_dev->private = wiichuck;
	poll_dev->poll = wiichuck_poll;
	poll_dev->poll_interval = 50; 
	poll_dev->open = wiichuck_open;

	input_dev = poll_dev->input;
	input_dev->name = "Nintendo Nunchuck";
	input_dev->id.bustype = BUS_I2C;
	input_dev->dev.parent = &client->dev;

	set_bit(EV_ABS, input_dev->evbit);
	set_bit(ABS_X, input_dev->absbit);
	set_bit(ABS_Y, input_dev->absbit);
	set_bit(ABS_RX, input_dev->absbit);
	set_bit(ABS_RY, input_dev->absbit);
	set_bit(ABS_RZ, input_dev->absbit);

	set_bit(EV_KEY, input_dev->evbit);
	set_bit(BTN_C, input_dev->keybit);
	set_bit(BTN_Z, input_dev->keybit);

	input_set_abs_params(input_dev, ABS_X, 30, 220, 4, 8);
	input_set_abs_params(input_dev, ABS_Y, 40, 200, 4, 8);
	input_set_abs_params(input_dev, ABS_RX, 0, 0x3ff, 4, 8);
	input_set_abs_params(input_dev, ABS_RY, 0, 0x3ff, 4, 8);
	input_set_abs_params(input_dev, ABS_RZ, 0, 0x3ff, 4, 8);

	rc = input_register_polled_device(wiichuck->poll_dev);
	if (rc) {
		dev_err(&client->dev, "Failed to register input device\n");
		goto err_register;
	}

	i2c_set_clientdata(client, wiichuck);

	return 0;

 err_register:
	input_free_polled_device(poll_dev);
 err_alloc:
	kfree(wiichuck);

	return rc;
}

static int wiichuck_remove(struct i2c_client *client)
{
	struct wiichuck_device *wiichuck = i2c_get_clientdata(client);

	i2c_set_clientdata(client, NULL);
	input_unregister_polled_device(wiichuck->poll_dev);
	input_free_polled_device(wiichuck->poll_dev);
	kfree(wiichuck);

	return 0;
}

static const struct i2c_device_id wiichuck_id[] = {
	{ "wiichuck", 0 },
	{ }
};

MODULE_DEVICE_TABLE(i2c, wiichuck_id);

static struct i2c_driver wiichuck_driver = {
	.driver = {
		.name = "wiichuck",
		.owner = THIS_MODULE,
	},
	.probe		= wiichuck_probe,
	.remove		= wiichuck_remove,
	.id_table	= wiichuck_id,
};

static int __init wiichuck_init(void)
{
	return i2c_add_driver(&wiichuck_driver);
}
module_init(wiichuck_init);
