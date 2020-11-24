#include <zephyr.h>
#include <device.h>
#include <stdio.h>
#include <stdlib.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/gatt.h>
#include <bluetooth/uuid.h>
#include <bluetooth/gatt_dm.h>
#include <bluetooth/scan.h>
#include <bluetooth/hci.h>

#include <dk_buttons_and_leds.h>

#include "ble.h"

struct sensor_data inside = { .name = "", .temperature = -127.0, .fresh = false, .rssi = -140 };
struct sensor_data outside = { .name = "", .temperature = -127.0, .fresh = false, .rssi = -140 };

uint16_t scanDurationSeconds = CONFIG_BLE_SCAN_DURATION_MINUTES * 60;
uint16_t scanPauseSeconds = CONFIG_BLE_SCAN_PAUSE_MINUTES * 60;
static struct k_delayed_work enable_scan_work;
static struct k_delayed_work disable_scan_work;

static bool adv_data_found(struct bt_data *data, void *user_data)
{
	struct sensor_data *sensorData = user_data;
	
	switch (data->type) {
	case BT_DATA_NAME_SHORTENED:
	case BT_DATA_NAME_COMPLETE:
		(void)memset(sensorData->name, 0, sizeof(sensorData->name));
		memcpy(sensorData->name, data->data,
		       MIN(data->data_len, sizeof(sensorData->name) - 1));
		return true;
	case BT_DATA_SVC_DATA16:
		sensorData->temperature = ((data->data[3] << 8) | data->data[2]) / 100.0;
		sensorData->fresh = true;
		return true;
	default:
		return true;
	}
}

static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t adv_type,
		    struct net_buf_simple *ad)
{
	char le_addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(addr, le_addr, sizeof(le_addr));
	if (strcmp(le_addr, "f0:49:04:8f:16:e5 (random)") == 0) {
		bt_data_parse(ad, adv_data_found, &outside);
		outside.rssi = rssi;
		printf("<BLE> %s (%ddBm) %f\n", outside.name,
		       rssi, outside.temperature);
		dk_set_led(DK_LED2, true);
	} else if (strcmp(le_addr, "d6:6f:5e:2f:a3:81 (random)") == 0) {
		bt_data_parse(ad, adv_data_found, &inside);
		inside.rssi = rssi;
		printf("<BLE> %s (%ddBm) %f\n", inside.name,
		       rssi, inside.temperature );
		dk_set_led(DK_LED1, true);
	}

	if (inside.fresh && outside.fresh) {
		printf("<BLE> Inside (%f) and Outside (%f) updated!\n", inside.temperature, outside.temperature);
		k_delayed_work_cancel(&disable_scan_work);
		k_delayed_work_submit(&disable_scan_work, K_NO_WAIT);
	}
}

static void scan_start(void)
{
	inside.fresh = false;
	inside.rssi = -140;
	outside.fresh = false;
	outside.rssi = -140;
	dk_set_led(DK_LED1, false);
	dk_set_led(DK_LED2, false);

	struct bt_le_scan_param scan_param = {
		.type = BT_HCI_LE_SCAN_PASSIVE,
		.options = BT_LE_SCAN_OPT_NONE,
		.interval = 0x0060,
		.window = 0x0060,
	};

	int err = bt_le_scan_start(&scan_param, scan_cb);
	if (err) {
		printk("<BLE> Starting scanning failed (err %d)\n", err);
		return;
	}
}

static void ble_ready(int err)
{
	printk("<BLE> Bluetooth initialized.\n");
	k_delayed_work_submit(&enable_scan_work, K_NO_WAIT);
}

static void enable_scan_work_fn(struct k_work *work)
{
	printk("<BLE> Scanning for %d seconds...\n", scanDurationSeconds);
	scan_start();
	k_delayed_work_submit(&disable_scan_work, K_SECONDS(scanDurationSeconds));
}

static void scan_stop(void)
{
	int err = bt_le_scan_stop();
	if (err) {
		printk("<BLE> Stopping scanning failed (err %d)\n", err);
		return;
	}
}

static void disable_scan_work_fn(struct k_work *work)
{
	printk("<BLE> Disabling scanning for %d seconds...\n", scanPauseSeconds);
	scan_stop();
	k_delayed_work_submit(&enable_scan_work, K_SECONDS(scanPauseSeconds));
}

static void work_init(void)
{
	k_delayed_work_init(&enable_scan_work, enable_scan_work_fn);
	k_delayed_work_init(&disable_scan_work, disable_scan_work_fn);
}

void beacons_init()
{
	work_init();
	printk("<BLE> Initializing...\n");
	int err = bt_enable(ble_ready);
	if (err) {
		printk("<BLE> Init failed (err %d)\n", err);
		return;
	}
}