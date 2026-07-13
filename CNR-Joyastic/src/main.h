#ifndef MAIN_H
#define MAIN_H

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <zephyr/logging/log.h>

#include <dk_buttons_and_leds.h>

#include <zephyr/net/wifi.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_mgmt.h>
#include <net/wifi_mgmt_ext.h>
#include <zephyr/net/wifi_credentials.h>

/* STEP 2 - Include the header files for Bluetooth LE */
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <net/wifi_prov_core/wifi_prov_core.h>
#include <bluetooth/services/wifi_provisioning.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <dk_buttons_and_leds.h>

#define BUTTON0_NODE DT_ALIAS(sw0)
#define BUTTON1_NODE DT_ALIAS(sw1)
#define EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)

static struct net_mgmt_event_callback mgmt_cb;
static bool wifi_connected;

static struct k_timer my_timer;
static uint32_t timer_count = 0;


#define ADV_DATA_UPDATE_INTERVAL      5

#define ADV_PARAM_UPDATE_DELAY        1

/* STEP 3.2 - Define indexes for accessing prov_svc_data */
#define ADV_DATA_VERSION_IDX	      (BT_UUID_SIZE_128 + 0)
#define ADV_DATA_FLAG_IDX	      (BT_UUID_SIZE_128 + 1)
#define ADV_DATA_FLAG_PROV_STATUS_BIT BIT(0)
#define ADV_DATA_FLAG_CONN_STATUS_BIT BIT(1)
#define ADV_DATA_RSSI_IDX	      (BT_UUID_SIZE_128 + 3)

#define PROV_BT_LE_ADV_PARAM_FAST BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, \
						BT_GAP_ADV_FAST_INT_MIN_2, \
						BT_GAP_ADV_FAST_INT_MAX_2, NULL)

#define PROV_BT_LE_ADV_PARAM_SLOW BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, \
						BT_GAP_ADV_SLOW_INT_MIN, \
						BT_GAP_ADV_SLOW_INT_MAX, NULL)

#define ADV_DAEMON_STACK_SIZE 4096
#define ADV_DAEMON_PRIORITY   5

K_THREAD_STACK_DEFINE(adv_daemon_stack_area, ADV_DAEMON_STACK_SIZE);
static struct k_work_q adv_daemon_work_q;

/* STEP 3.1 Define an array for storing provisioning service data */
static uint8_t prov_svc_data[] = {BT_UUID_PROV_VAL, 0x00, 0x00, 0x00, 0x00};

/* STEP 4.1 Define a variable for the device name */
static uint8_t device_name[] = {'C', 'N', 'R', '-', 'W', 'I', 'F', 'I'};

/* STEP 4.2 - Define the data structure for the advertisement packet */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_PROV_VAL),
	BT_DATA(BT_DATA_NAME_COMPLETE, device_name, sizeof(device_name)),
};

/* STEP 4.3 - Define the data structure for the scan response packet */
static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_SVC_DATA128, prov_svc_data, sizeof(prov_svc_data)),
};

/* STEP 6 - Define the work structures for updating advertisement parameters and data */
static struct k_work_delayable update_adv_param_work;
static struct k_work_delayable update_adv_data_work;


/* TCP */
#include <zephyr/net/socket.h>

#define SERVER_IP    "192.168.40.233"
#define SERVER_PORT  2444
#define MESSAGE_SIZE 256

/* TCP globals */
static int tcp_sock;
static uint8_t recv_buf[MESSAGE_SIZE];
static bool tcp_connected = false;

#endif /*MAIN_H*/