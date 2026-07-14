/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "main.h"

LOG_MODULE_REGISTER(Lesson2_Exercise3, LOG_LEVEL_INF);

static const struct gpio_dt_spec button0 = GPIO_DT_SPEC_GET(BUTTON0_NODE, gpios);
static const struct gpio_dt_spec button1 = GPIO_DT_SPEC_GET(BUTTON1_NODE, gpios);

static void timer_expiry_fn(struct k_timer *timer)
{
    timer_count++;
}

/* Called when timer is stopped */
static void timer_stop_fn(struct k_timer *timer)
{
    LOG_INF("Timer stopped. Total count: %d", timer_count);
}

static void button_handler(uint32_t button_state,
                           uint32_t has_changed)
{
    if (has_changed & DK_BTN1_MSK) {
        if (tcp_connected) {
			dk_set_led_on(DK_LED2);
            char *msg = "PINETICS 1nd branch--->";
            int err = zsock_send(tcp_sock, msg, strlen(msg), 0);
            if (err < 0) {
                LOG_ERR("TCP send failed, err: %d", errno);
            } else {
                LOG_INF("TCP sent: %s", msg);
            }
			dk_set_led_off(DK_LED2);
        }
    }

    if (has_changed & DK_BTN2_MSK) {
        // printk("Button 2\n");
		 if (tcp_connected) {
			dk_set_led_on(DK_LED2);
            char *msg = "PINETICS 2nd branch--->";
            int err = zsock_send(tcp_sock, msg, strlen(msg), 0);
            if (err < 0) {
                LOG_ERR("TCP send failed, err: %d", errno);
            } else {
                LOG_INF("TCP sent: %s", msg);
            }
			dk_set_led_off(DK_LED2);
        }
    }
}


static void net_mgmt_event_handler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
				   struct net_if *iface)
{
	if ((mgmt_event & EVENT_MASK) != mgmt_event) {
		return;
	}
	if (mgmt_event == NET_EVENT_L4_CONNECTED) {
		LOG_INF("Network connected");
		wifi_connected = true;
		dk_set_led_on(DK_LED1);
		return;
	}
	if (mgmt_event == NET_EVENT_L4_DISCONNECTED) {
		if (wifi_connected == false) {
			LOG_INF("Waiting for network to be connected");
		} else {
			dk_set_led_off(DK_LED1);
			LOG_INF("Network disconnected");
			wifi_connected = false;
		}
		return;
	}
}

static void update_wifi_status_in_adv(void)
{

	/* STEP 5.1 - Update the firmware version*/
	prov_svc_data[ADV_DATA_VERSION_IDX] = PROV_SVC_VER;

	/* STEP 5.2 - Update the provisioning state */
	if (!wifi_prov_state_get()) {
		prov_svc_data[ADV_DATA_FLAG_IDX] &= ~ADV_DATA_FLAG_PROV_STATUS_BIT;
	} else {
		prov_svc_data[ADV_DATA_FLAG_IDX] |= ADV_DATA_FLAG_PROV_STATUS_BIT;
	}

	/* STEP 5.3 - Update the Wi-Fi connection status*/
	struct net_if *iface = net_if_get_first_wifi();
	struct wifi_iface_status status = {0};

	int err = net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &status,
			   sizeof(struct wifi_iface_status));
	if ((err != 0) || (status.state < WIFI_STATE_ASSOCIATED)) {
		prov_svc_data[ADV_DATA_FLAG_IDX] &= ~ADV_DATA_FLAG_CONN_STATUS_BIT;
		prov_svc_data[ADV_DATA_RSSI_IDX] = INT8_MIN;
	} else {
		prov_svc_data[ADV_DATA_FLAG_IDX] |= ADV_DATA_FLAG_CONN_STATUS_BIT;
		prov_svc_data[ADV_DATA_RSSI_IDX] = status.rssi;
	}
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (err) {
		LOG_ERR("BT Connection failed (err 0x%02x).\n", err);
		return;
	}

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("BT Connected: %s", addr);

	/* STEP 8.1 - Upon a connected event, cancel update_adv_data_work */
	k_work_cancel_delayable(&update_adv_data_work);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("BT Disconnected: %s (reason 0x%02x).\n", addr, reason);

	/* STEP 8.2 - Upon a disconnected event, reschedule all work items*/
	k_work_reschedule_for_queue(&adv_daemon_work_q, &update_adv_param_work,
				    K_SECONDS(ADV_PARAM_UPDATE_DELAY));
	k_work_reschedule_for_queue(&adv_daemon_work_q, &update_adv_data_work, K_NO_WAIT);
}

static void identity_resolved(struct bt_conn *conn, const bt_addr_le_t *rpa,
				const bt_addr_le_t *identity)
{
	char addr_identity[BT_ADDR_LE_STR_LEN];
	char addr_rpa[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(identity, addr_identity, sizeof(addr_identity));
	bt_addr_le_to_str(rpa, addr_rpa, sizeof(addr_rpa));

	LOG_INF("BT Identity resolved %s -> %s.\n", addr_rpa, addr_identity);
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
				enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (!err) {
		LOG_INF("BT Security changed: %s level %u.\n", addr, level);
	} else {
		LOG_ERR("BT Security failed: %s level %u err %d.\n", addr, level, err);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.identity_resolved = identity_resolved,
	.security_changed = security_changed,
};

static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("BT Pairing cancelled: %s.\n", addr);
}

static struct bt_conn_auth_cb auth_cb_display = {
	.cancel = auth_cancel,
};

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("BT pairing completed: %s, bonded: %d\n", addr, bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	LOG_INF("BT Pairing Failed (%d). Disconnecting.\n", reason);
	bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
}

static struct bt_conn_auth_info_cb auth_info_cb_display = {

	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};

static void update_adv_data_task(struct k_work *item)
{
	/* STEP 7.2 - Update the advertising and scan response data*/
	int err;

	update_wifi_status_in_adv();
	err = bt_le_adv_update_data(ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err != 0) {
		LOG_INF("Cannot update advertisement data, err = %d\n", err);
	}
	k_work_reschedule_for_queue(&adv_daemon_work_q, &update_adv_data_work,
				    K_SECONDS(ADV_DATA_UPDATE_INTERVAL));
}

static void update_adv_param_task(struct k_work *item)
{
	/* STEP 7.1 - Stop advertising, then start advertising again */
	int err;

	err = bt_le_adv_stop();
	if (err != 0) {
		LOG_ERR("Cannot stop advertisement: err = %d\n", err);
		return;
	}

	err = bt_le_adv_start(prov_svc_data[ADV_DATA_FLAG_IDX] & ADV_DATA_FLAG_PROV_STATUS_BIT
				      ? PROV_BT_LE_ADV_PARAM_SLOW
				      : PROV_BT_LE_ADV_PARAM_FAST,
			      ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err != 0) {
		LOG_ERR("Cannot start advertisement: err = %d\n", err);
	}
}

static void byte_to_hex(char *ptr, uint8_t byte, char base)
{
	int i, val;

	for (i = 0, val = (byte & 0xf0) >> 4; i < 2; i++, val = byte & 0x0f) {
		if (val < 10) {
			*ptr++ = (char) (val + '0');
		} else {
			*ptr++ = (char) (val - 10 + base);
		}
	}
}

static void update_dev_name(struct net_linkaddr *mac_addr)
{
	byte_to_hex(&device_name[2], mac_addr->addr[3], 'A');
	byte_to_hex(&device_name[4], mac_addr->addr[4], 'A');
	byte_to_hex(&device_name[6], mac_addr->addr[5], 'A');
}
static int tcp_connect(void)
{
    int err;
    struct sockaddr_in server_addr;

    tcp_sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (tcp_sock < 0) {
        LOG_ERR("Failed to create TCP socket, err: %d", errno);
        return -errno;
    }
    LOG_INF("TCP socket created");

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(SERVER_PORT);
    zsock_inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

    err = zsock_connect(tcp_sock,
                        (struct sockaddr *)&server_addr,
                        sizeof(server_addr));
    if (err < 0) {
        LOG_ERR("Failed to connect, err: %d", errno);
        zsock_close(tcp_sock);
        return -errno;
    }

    LOG_INF("TCP connected to %s:%d", SERVER_IP, SERVER_PORT);
    tcp_connected = true;
    return 0;
}
int main(void)
{
	int err;
	
	if (dk_leds_init() != 0) {
		LOG_ERR("Failed to initialize the LED library");
	}

	/* Sleep 1 seconds to allow initialization of wifi driver. */
	k_sleep(K_SECONDS(1));

	net_mgmt_init_event_callback(&mgmt_cb, net_mgmt_event_handler, EVENT_MASK);
	net_mgmt_add_event_callback(&mgmt_cb);

	bt_conn_auth_cb_register(&auth_cb_display);
	bt_conn_auth_info_cb_register(&auth_info_cb_display);

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d).\n", err);
		return 0;
	}
	LOG_INF("Bluetooth initialized.\n");

	/* STEP 9 - Enable the Bluetooth Wi-Fi Provisioning Service */
	err = wifi_prov_init();
	if (err == 0) {
		LOG_INF("Wi-Fi provisioning service starts successfully.\n");
	} else {
		LOG_ERR("Error occurs when initializing Wi-Fi provisioning service.\n");
		return 0;
	}

	/* STEP 10.1 Prepare the advertisement data */
	struct net_if *iface = net_if_get_default();
	// struct net_linkaddr *mac_addr = net_if_get_link_addr(iface);
	char device_name_str[sizeof(device_name) + 1];

	// if (mac_addr) {
	// 	update_dev_name(mac_addr);
	// }
	device_name_str[sizeof(device_name_str) - 1] = '\0';
	memcpy(device_name_str, device_name, sizeof(device_name));
	bt_set_name(device_name_str);

	/* STEP 10.2 - Start advertising */
	update_wifi_status_in_adv();

	err = bt_le_adv_start(prov_svc_data[ADV_DATA_FLAG_IDX] & ADV_DATA_FLAG_PROV_STATUS_BIT
				      ? PROV_BT_LE_ADV_PARAM_SLOW
				      : PROV_BT_LE_ADV_PARAM_FAST,
			      ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("BT Advertising failed to start (err %d)\n", err);
		return 0;
	}
	LOG_INF("BT Advertising successfully started.\n");

	k_work_queue_init(&adv_daemon_work_q);
	k_work_queue_start(&adv_daemon_work_q, adv_daemon_stack_area,
			K_THREAD_STACK_SIZEOF(adv_daemon_stack_area), ADV_DAEMON_PRIORITY,
			NULL);

	/* STEP 11 - Initializa all work items to their respective task */
	k_work_init_delayable(&update_adv_param_work, update_adv_param_task);
	k_work_init_delayable(&update_adv_data_work, update_adv_data_task);
	k_work_schedule_for_queue(&adv_daemon_work_q, &update_adv_data_work,
				  K_SECONDS(ADV_DATA_UPDATE_INTERVAL));

	// if (!gpio_is_ready_dt(&button0)) {
    //     printk("Error: button0 not ready\n");
    //     return -1;
    // }

	// gpio_pin_configure_dt(&button0, GPIO_INPUT);

	//  if (!gpio_is_ready_dt(&button1)) {
    //     printk("Error: button1 not ready\n");
    //     return -1;
    // }
    // gpio_pin_configure_dt(&button1, GPIO_INPUT);

	dk_buttons_init(button_handler);

		 /* Initialize timer */
	k_timer_init(&my_timer, timer_expiry_fn, timer_stop_fn);

	/* Start timer - fires every 1 second, first fire after 1 second */
	k_timer_start(&my_timer, K_MSEC(1), K_MSEC(1));

	LOG_INF("Software timer started");

	/* Apply stored WiFi credentials */
    net_mgmt(NET_REQUEST_WIFI_CONNECT_STORED, iface, NULL, 0);

    /* Wait for WiFi then connect TCP */
    while (!wifi_connected) {
        k_sleep(K_MSEC(500));
        LOG_INF("Waiting for WiFi...");
    }

	LOG_INF("WiFi connected! Waiting for IP address...");
    k_sleep(K_SECONDS(3));

    if (tcp_connect() != 0) {
        LOG_ERR("TCP connection failed");
    }

	while (1) {
		// int val0 = gpio_pin_get_dt(&button0);
 		// int val1 = gpio_pin_get_dt(&button1);

        // if (val0 == 1) {
        //     printk("Button 1 pressed!\n");
        // }
		// if (val1 == 1) {
        //     printk("Button 2 pressed!\n");
        // }

		// if(timer_count <= 1000){			
		// 	dk_set_led_on(DK_LED2);
		// 	LOG_INF("TImer 1 seconde is completed ON.... !!!!!");
		// }else if(timer_count > 1000){
		// 	dk_set_led_off(DK_LED2);
		// 	LOG_INF("TImer 1 seconde is completed OFF.... !!!!!");
		// 	if(timer_count >= 2000)
		// 		timer_count = 0;
		// }

		if (tcp_connected) {
            struct zsock_pollfd fds = {
                .fd     = tcp_sock,
                .events = ZSOCK_POLLIN,
            };
            int ret = zsock_poll(&fds, 1, 0);
            if (ret > 0 && (fds.revents & ZSOCK_POLLIN)) {
                int received = zsock_recv(tcp_sock,
                                          recv_buf,
                                          sizeof(recv_buf) - 1,
                                          0);
                if (received > 0) {
                    recv_buf[received] = '\0';
                    LOG_INF("TCP received: %s", recv_buf);
                } else if (received == 0) {
                    LOG_INF("Server disconnected");
                    tcp_connected = false;
                    zsock_close(tcp_sock);
                }
            }
        }//else {
		// 	   LOG_INF("TCP not connected \n");
		// }

		
		k_sleep(K_MSEC(1)); 
	}

	/* STEP 12 - Apply stored Wi-Fi credentials */
	// net_mgmt(NET_REQUEST_WIFI_CONNECT_STORED, iface, NULL, 0);

	return 0;
}