#include <zephyr.h>
#include <device.h>
#include <stdlib.h>
#include <cJSON.h>
#include <cJSON_os.h>
#include <net/aws_iot.h>
#include <power/reboot.h>
#include <date_time.h>
#include <dfu/mcuboot.h>
#include <modem/lte_lc.h>
#include <modem/bsdlib.h>
#include <modem/at_cmd.h>
#include <modem/at_notif.h>
#include <modem/modem_info.h>
#include <bsd.h>
#include <modem/at_cmd.h>
#include <math.h>
#include <dk_buttons_and_leds.h>

#include "cloud.h"
#include "ble.h"

static struct k_delayed_work report_state_work;
static struct k_delayed_work leds_update_work;
static struct k_delayed_work connect_work;

/* Interval in milliseconds between each time status LEDs are updated. */
#define LEDS_UPDATE_INTERVAL K_MSEC(500)

/* Interval in seconds between reconnect attempts */
#define RECONNECT_INTERVAL K_SECONDS(CONFIG_PUBLISH_CHANGES_INTERVAL_MINUTES * 60)

K_SEM_DEFINE(lte_connected, 0, 1);
static struct desired_state desiredCfg = { 
};

static struct current_state currentState = {
};

static struct track_reported trackReported = {
	.publishVersion = true,
	.inside = -127.0,
	.outside = -127.0,
};

bool isConnected = false;

static bool needsPublish() {
	if (trackReported.publishVersion) return true;
	if (fabs(inside.temperature - trackReported.inside) > TEMPERATURE_THRESHOLD_CELSIUS) return true;
	if (fabs(outside.temperature - trackReported.outside) > TEMPERATURE_THRESHOLD_CELSIUS) return true;
	return false;
}

static void report_state_work_fn(struct k_work *work)
{
	// Schedule next publication
	k_delayed_work_submit(&report_state_work, RECONNECT_INTERVAL);

	if(!isConnected) {
		printk("Not connected to AWS IoT.\n");
		return;
	}

	if (!needsPublish()) {
		printk("No updates to report.\n");
		return;
	}

	dk_set_led(DK_LED3, true);
	int err = cloud_report_state(&currentState, &trackReported);
	if (err) {
		printk("cloud_report_state, error: %d\n", err);
		return;
	}
	dk_set_led(DK_LED3, false);
}

void aws_iot_event_handler(const struct aws_iot_evt *const evt)
{
	int err;

	switch (evt->type) {
	case AWS_IOT_EVT_CONNECTING:
		printf("<CLOUD> Connecting to AWS IoT...\n");
		break;
	case AWS_IOT_EVT_CONNECTED:
		printf("<CLOUD> Connected to AWS IoT.\n");
		isConnected = true;
		k_delayed_work_cancel(&connect_work);

		if (evt->data.persistent_session) {
			printk("<CLOUD> Persistent session enabled\n");
		}

		/** Successfully connected to AWS IoT broker, mark image as
		 *  working to avoid reverting to the former image upon reboot.
		 */
		boot_write_img_confirmed();

		err = lte_lc_psm_req(true);
		if (err) {
			printk("<CLOUD> Requesting PSM failed, error: %d\n", err);
		}
		break;
	case AWS_IOT_EVT_READY:
		printf("<CLOUD> Subscribed to all topics.\n");
		/** Send version number to AWS IoT broker to verify that the
		 *  FOTA update worked.
		 */
		k_delayed_work_submit(&report_state_work, K_NO_WAIT);
		break;
	case AWS_IOT_EVT_DISCONNECTED:
		printf("<CLOUD> Disconnected from AWS IoT.\n");
		k_delayed_work_cancel(&report_state_work);
		isConnected = false;
		// Trigger a reconnect
		k_delayed_work_submit(&connect_work, RECONNECT_INTERVAL);
		break;
	case AWS_IOT_EVT_DATA_RECEIVED:
		printk("<CLOUD> AWS_IOT_EVT_DATA_RECEIVED\n");
		err = cloud_decode_response(evt->data.msg.ptr, &desiredCfg);
		if (err) {
			printk("Could not decode response %d", err);
		}
		break;
	case AWS_IOT_EVT_FOTA_START:
		printk("<CLOUD> AWS_IOT_EVT_FOTA_START\n");
		break;
	case AWS_IOT_EVT_FOTA_ERASE_PENDING:
		printk("<CLOUD> AWS_IOT_EVT_FOTA_ERASE_PENDING\n");
		printk("<CLOUD> Disconnect LTE link or reboot\n");
		err = lte_lc_offline();
		if (err) {
			printk("<CLOUD> Error disconnecting from LTE\n");
		}
		break;
	case AWS_IOT_EVT_FOTA_ERASE_DONE:
		printk("<CLOUD> AWS_FOTA_EVT_ERASE_DONE\n");
		printk("<CLOUD> Reconnecting the LTE link");
		err = lte_lc_connect();
		if (err) {
			printk("<CLOUD> Error connecting to LTE\n");
		}
		break;
	case AWS_IOT_EVT_FOTA_DONE:
		printk("<CLOUD> AWS_IOT_EVT_FOTA_DONE\n");
		printk("<CLOUD> FOTA done, rebooting device\n");
		aws_iot_disconnect();
		sys_reboot(0);
		break;
	case AWS_IOT_EVT_FOTA_DL_PROGRESS:
		printk("<CLOUD> AWS_IOT_EVT_FOTA_DL_PROGRESS, (%d%%)", evt->data.fota_progress);
	case AWS_IOT_EVT_ERROR:
		printk("<CLOUD> AWS_IOT_EVT_ERROR, %d\n", evt->data.err);
		break;
	default:
		printk("<CLOUD> Unknown AWS IoT event type: %d\n", evt->type);
		break;
	}
}

static void lte_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if ((evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_HOME) &&
		     (evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_ROAMING)) {
			break;
		}

		printk("Network registration status: %s\n",
			evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ?
			"Connected - home network" : "Connected - roaming");

		k_sem_give(&lte_connected);
		break;
	case LTE_LC_EVT_PSM_UPDATE:
		printk("PSM parameter update: TAU: %d, Active time: %d\n",
			evt->psm_cfg.tau, evt->psm_cfg.active_time);
		break;
	case LTE_LC_EVT_EDRX_UPDATE: {
		char log_buf[60];
		ssize_t len;

		len = snprintf(log_buf, sizeof(log_buf),
			       "eDRX parameter update: eDRX: %f, PTW: %f",
			       evt->edrx_cfg.edrx, evt->edrx_cfg.ptw);
		if (len > 0) {
			printk("%s\n", log_buf);
		}
		break;
	}
	case LTE_LC_EVT_RRC_UPDATE:
		// printk("RRC mode: %s\n", evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED ? "Connected" : "Idle");
		break;
	case LTE_LC_EVT_CELL_UPDATE:
		printk("LTE cell changed: Cell ID: %d, Tracking area: %d\n",
			evt->cell.id, evt->cell.tac);
		break;
	default:
		break;
	}
}

static void modem_configure(void)
{
	int err = lte_lc_init_and_connect_async(lte_handler);
	if (err) {
		printk("Modem could not be configured, error: %d\n", err);
	}
}

static int bsd_lib_modem_dfu_handler(void)
{
	int err;

	err = bsdlib_init();

	switch (err) {
	case MODEM_DFU_RESULT_OK:
		printk("Modem update suceeded, reboot\n");
		sys_reboot(SYS_REBOOT_COLD);
		break;
	case MODEM_DFU_RESULT_UUID_ERROR:
	case MODEM_DFU_RESULT_AUTH_ERROR:
		printk("Modem update failed, error: %d\n", err);
		printk("Modem will use old firmware\n");
		sys_reboot(SYS_REBOOT_COLD);
		break;
	case MODEM_DFU_RESULT_HARDWARE_ERROR:
	case MODEM_DFU_RESULT_INTERNAL_ERROR:
		printk("Modem update malfunction, error: %d, reboot\n", err);
		sys_reboot(SYS_REBOOT_COLD);
		break;
	default:
		break;
	}

	err = at_notif_init();
	__ASSERT(err == 0, "AT Notify could not be initialized.");
	err = at_cmd_init();
	__ASSERT(err == 0, "AT CMD could not be established.");
	return err;
}

/**@brief Update LEDs state. */
static void leds_update(struct k_work *work)
{
	static bool led_on;
	if (!isConnected) {
		dk_set_led(DK_LED4, false);
	} else {
		led_on = !led_on;
		dk_set_led(DK_LED4, led_on);
	}
	k_delayed_work_submit(&leds_update_work, LEDS_UPDATE_INTERVAL);
}

static void connect_work_fn(struct k_work *work)
{
	int err = aws_iot_connect(NULL);
	if (err) {
		printk("<CLOUD> aws_iot_connect failed: %d\n", err);
	}
	// If connect fails this will trigger a reconnect
	k_delayed_work_submit(&connect_work, RECONNECT_INTERVAL);
}

static void work_init(void)
{
	k_delayed_work_init(&report_state_work, report_state_work_fn);
	k_delayed_work_init(&leds_update_work, leds_update);
	k_delayed_work_submit(&leds_update_work, LEDS_UPDATE_INTERVAL);
	k_delayed_work_init(&connect_work, connect_work_fn);
}

void main(void) {
	int err;

	printf("##########################################################################################\n");
	printf(" Version:                   %s\n", CONFIG_APP_VERSION);
	printf(" AWS IoT Client ID:         %s\n", CONFIG_AWS_IOT_CLIENT_ID_STATIC);
	printf(" AWS IoT broker hostname:   %s\n", CONFIG_AWS_IOT_BROKER_HOST_NAME);
	printf(" Reconnect interval:        %d minutes\n", CONFIG_RECONNECT_INTERVAL_MINUTES);
	printf(" Publish changes every:     %d minutes\n", CONFIG_PUBLISH_CHANGES_INTERVAL_MINUTES);
	printf(" BLE Scan Interval:         %d minutes\n", CONFIG_BLE_SCAN_DURATION_MINUTES);
	printf(" BLE Scan Pause:            %d minutes\n", CONFIG_BLE_SCAN_PAUSE_MINUTES);
	printf(" Temperature threshold:     %f celsius\n", TEMPERATURE_THRESHOLD_CELSIUS);
	printf("##########################################################################################\n");

	err = dk_leds_init();
	if (err) {
		printf("ledError, error: %d", err);
		return;
	}

	beacons_init();

	cJSON_Init();

	err = bsd_lib_modem_dfu_handler();
	if (err) {
		printk("DFU handler could not be initialized, error: %d\n", err);
		return;
	}

	work_init();

	err = lte_lc_init_and_connect_async(lte_handler);
	if (err) {
		printk("Modem could not be configured, error: %d\n", err);
	}

	printk("Initializing modem ...\n");
	err = modem_info_init();
	if (err) {
		printk("Failed initializing modem info module, error: %d\n",
			err);
	}
	k_sem_take(&lte_connected, K_FOREVER);

	date_time_update_async(NULL);

	err = aws_iot_init(NULL, aws_iot_event_handler);
	if (err) {
		printk("AWS IoT library could not be initialized, error: %d\n", err);
		return;
	}

	// Sleep to ensure that time has been obtained before communication with AWS IoT.
	printf("Waiting 15 seconds before attempting to connect...\n");
	k_delayed_work_submit(&connect_work, K_SECONDS(15));
}
