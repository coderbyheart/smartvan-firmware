#include <zephyr.h>
#include <device.h>
#include <stdio.h>
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

#include "cloud.h"

static struct k_delayed_work report_state_work;

K_SEM_DEFINE(lte_connected, 0, 1);

static struct desired_state desiredCfg = { 
};

static struct current_state currentState = {
};

static struct track_reported trackReported = {
	.publishVersion = true
};

static struct track_desired trackDesired = {
};

bool isConnected = false;

static bool needsPublish() {
	if (trackReported.publishVersion) return true;
	return false;
}

static void report_state_work_fn(struct k_work *work)
{
	if (!needsPublish()) {
		printk("No updates to report.\n");
	} else if(!isConnected) {
		printk("Not connected to AWS IoT.\n");
	} else {
		int err;
		err = cloud_report_state(&currentState, &trackReported, &trackDesired);
		if (err) {
			printk("cloud_report_state, error: %d\n", err);
		}
	}

	// Schedule next publication
	k_delayed_work_submit(&report_state_work, K_SECONDS(CONFIG_PUBLISH_INTERVAL_MINUTES * 60));
}

void aws_iot_event_handler(const struct aws_iot_evt *const evt)
{
	int err;

	switch (evt->type) {
	case AWS_IOT_EVT_CONNECTING:
		printf("Connecting to AWS IoT...\n");
		break;
	case AWS_IOT_EVT_CONNECTED:
		printf("Connected to AWS IoT.\n");
		isConnected = true;

		if (evt->data.persistent_session) {
			printk("Persistent session enabled\n");
		}

		/** Successfully connected to AWS IoT broker, mark image as
		 *  working to avoid reverting to the former image upon reboot.
		 */
		boot_write_img_confirmed();

		err = lte_lc_psm_req(true);
		if (err) {
			printk("Requesting PSM failed, error: %d\n", err);
		}
		break;
	case AWS_IOT_EVT_READY:
		printf("Subscribed to all topics.\n");
		/** Send version number to AWS IoT broker to verify that the
		 *  FOTA update worked.
		 */
		k_delayed_work_submit(&report_state_work, K_NO_WAIT);
		break;
	case AWS_IOT_EVT_DISCONNECTED:
		printf("Disconnected from AWS IoT.\n");
		k_delayed_work_cancel(&report_state_work);
		isConnected = false;
		break;
	case AWS_IOT_EVT_DATA_RECEIVED:
		printk("AWS_IOT_EVT_DATA_RECEIVED\n");
		err = cloud_decode_response(evt->data.msg.ptr, &desiredCfg);
		if (err) {
			printk("Could not decode response %d", err);
		}
		break;
	case AWS_IOT_EVT_FOTA_START:
		printk("AWS_IOT_EVT_FOTA_START\n");
		break;
	case AWS_IOT_EVT_FOTA_ERASE_PENDING:
		printk("AWS_IOT_EVT_FOTA_ERASE_PENDING\n");
		printk("Disconnect LTE link or reboot\n");
		err = lte_lc_offline();
		if (err) {
			printk("Error disconnecting from LTE\n");
		}
		break;
	case AWS_IOT_EVT_FOTA_ERASE_DONE:
		printk("AWS_FOTA_EVT_ERASE_DONE\n");
		printk("Reconnecting the LTE link");
		err = lte_lc_connect();
		if (err) {
			printk("Error connecting to LTE\n");
		}
		break;
	case AWS_IOT_EVT_FOTA_DONE:
		printk("AWS_IOT_EVT_FOTA_DONE\n");
		printk("FOTA done, rebooting device\n");
		aws_iot_disconnect();
		sys_reboot(0);
		break;
	case AWS_IOT_EVT_FOTA_DL_PROGRESS:
		printk("AWS_IOT_EVT_FOTA_DL_PROGRESS, (%d%%)", evt->data.fota_progress);
	case AWS_IOT_EVT_ERROR:
		printk("AWS_IOT_EVT_ERROR, %d\n", evt->data.err);
		break;
	default:
		printk("Unknown AWS IoT event type: %d\n", evt->type);
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

static void at_configure(void)
{
	int err;

	err = at_notif_init();
	__ASSERT(err == 0, "AT Notify could not be initialized.");
	err = at_cmd_init();
	__ASSERT(err == 0, "AT CMD could not be established.");
}

static void bsd_lib_modem_dfu_handler(void)
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

	at_configure();
}

static int remove_whitespace(char *buf)
{
	size_t i, j = 0, len;

	len = strlen(buf);
	for (i = 0; i < len; i++) {
		if (buf[i] >= 32 && buf[i] <= 126) {
			if (j != i) {
				buf[j] = buf[i];
			}

			j++;
		}
	}

	if (j < len) {
		buf[j] = '\0';
	}

	return 0;
}

static int query_modem(const char *cmd, char *buf, size_t buf_len)
{
	int ret;
	enum at_cmd_state at_state;

	ret = at_cmd_write(cmd, buf, buf_len, &at_state);
	if (ret) {
		printk("at_cmd_write [%s] error:%d, at_state: %d\n",
			cmd, ret, at_state);
		strncpy(buf, "error", buf_len);
		return ret;
	}

	remove_whitespace(buf);
	return 0;
}

static void work_init(void)
{
	k_delayed_work_init(&report_state_work, report_state_work_fn);
}

void main(void) {
	int err;

	printf("##########################################################################################\n");
	printf("Version:                   %s\n", CONFIG_APP_VERSION);
	printf("AWS IoT Client ID:         %s\n", CONFIG_AWS_IOT_CLIENT_ID_STATIC);
	printf("AWS IoT broker hostname:   %s\n", CONFIG_AWS_IOT_BROKER_HOST_NAME);
	printf("Publish min interval:      %d seconds\n", CONFIG_PUBLISH_INTERVAL_MINUTES * 60);
	printf("##########################################################################################\n");

	cJSON_Init();

	bsd_lib_modem_dfu_handler();

	work_init();

	int awsErr = aws_iot_init(NULL, aws_iot_event_handler);
	if (awsErr) {
		printk("AWS IoT library could not be initialized, error: %d\n", awsErr);
		return;
	}

	modem_configure();

	printk("Initializing modem ...\n");
	err = modem_info_init();
	if (err) {
		printk("Failed initializing modem info module, error: %d\n",
			err);
	}
	k_sem_take(&lte_connected, K_FOREVER);

	date_time_update_async(NULL);
	// Sleep to ensure that time has been obtained before communication with AWS IoT.
	printf("Waiting 15 seconds before attempting to connect...\n");
	k_sleep(K_SECONDS(15));
	
	err = aws_iot_connect(NULL);
	if (err) {
		printk("aws_iot_connect failed: %d\n", err);
	}
}
