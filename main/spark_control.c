/*
 * Copyright (C) 2022 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL BLUEKITCHEN
 * GMBH OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */

#define BTSTACK_FILE__ "spark_control.c"

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
 
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "btstack.h"

// #define LOG_MESSAGES

static const char spark_40_device_name[]          = "Spark 40 BLE";
static uint16_t   spark_40_service_uuid           = 0xffc0;
static uint16_t   spark_40_characteristic_tx_uuid = 0xffc1;
static uint16_t   spark_40_characteristic_rx_uuid = 0xffc2;

static bd_addr_t                    spark_40_addr;
static uint8_t                      spark_40_addr_type;
static hci_con_handle_t             spark_40_connection_handle;
static gatt_client_service_t        spark_40_service;
static gatt_client_characteristic_t spark_40_characteristic_rx;
static gatt_client_characteristic_t spark_40_characteristic_tx;
static gatt_client_notification_t   spark_40_notification_listener;
static uint8_t                      spark_40_preset;

static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_packet_callback_registration_t sm_event_callback_registration;

static enum {
    APP_STATE_W4_SPARK_ADV,
    APP_STATE_W4_SERVICE,
    APP_STATE_W4_RX_CHARACTERISTIC,
    APP_STATE_W4_TX_CHARACTERISTIC,
    APP_STATE_W4_RX_SUBSCRIBED,
    APP_STATE_CONNECTED
} app_state;

static void process_update(const uint8_t * data, uint16_t len);
static void select_preset(uint8_t preset);

#ifdef ESP_PLATFORM

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "driver/gpio.h"
#include "led_strip_encoder.h"

#define RMT_LED_STRIP_RESOLUTION_HZ 10000000 // 10MHz resolution, 1 tick = 0.1us (led strip needs a high resolution)
#define RMT_LED_STRIP_GPIO_NUM      8

#define EXAMPLE_LED_NUMBERS         1
#define EXAMPLE_CHASE_SPEED_MS      1000

#define BUTTON_GPIO_NUM     10
#define BUTTON_POLL_PERIOD_MS 50

static const char *TAG = "example";

static uint8_t led_strip_pixels[EXAMPLE_LED_NUMBERS * 3];
static rmt_channel_handle_t led_chan = NULL;
static rmt_encoder_handle_t led_encoder = NULL;
static rmt_transmit_config_t tx_config = {
        .loop_count = 0, // no transfer loop
};

static btstack_timer_source_t button_poller;
static bool button_pressed;

static void set_led(uint8_t red, uint8_t green, uint8_t blue){
    led_strip_pixels[0] = green;
    led_strip_pixels[1] = red;
    led_strip_pixels[2] = blue;
    ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, sizeof(led_strip_pixels), &tx_config));
}

static void button_poll(btstack_timer_source_t * ts) {
    bool pressed = gpio_get_level(BUTTON_GPIO_NUM) != 0;
    if (pressed && !button_pressed){
        // button was pressed, select other preset
        select_preset(1 - (spark_40_preset & 1));
    }
    button_pressed = pressed;

    btstack_run_loop_set_timer(&button_poller, BUTTON_POLL_PERIOD_MS);
    btstack_run_loop_add_timer(&button_poller);
}

static void platform_init(void){
    // setup led strip
    ESP_LOGI(TAG, "Create RMT TX channel");
    rmt_tx_channel_config_t tx_chan_config = {
            .clk_src = RMT_CLK_SRC_DEFAULT, // select source clock
            .gpio_num = RMT_LED_STRIP_GPIO_NUM,
            .mem_block_symbols = 64, // increase the block size can make the LED less flickering
            .resolution_hz = RMT_LED_STRIP_RESOLUTION_HZ,
            .trans_queue_depth = 4, // set the number of transactions that can be pending in the background
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &led_chan));

    ESP_LOGI(TAG, "Install led strip encoder");
    led_strip_encoder_config_t encoder_config = {
            .resolution = RMT_LED_STRIP_RESOLUTION_HZ,
    };
    ESP_ERROR_CHECK(rmt_new_led_strip_encoder(&encoder_config, &led_encoder));

    ESP_LOGI(TAG, "Enable RMT TX channel");
    ESP_ERROR_CHECK(rmt_enable(led_chan));

    ESP_LOGI(TAG, "Start LED rainbow chase");

    // setup GPIOs
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = 1ULL << BUTTON_GPIO_NUM;
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);

    // poll button
    btstack_run_loop_set_timer_handler(&button_poller, &button_poll);
    btstack_run_loop_set_timer(&button_poller, BUTTON_POLL_PERIOD_MS);
    btstack_run_loop_add_timer(&button_poller);
}
#else
static void platform_init(void){}
static void set_led(uint8_t red, uint8_t green, uint8_t blue){}
#endif

static void start_scanning(void){
    app_state = APP_STATE_W4_SPARK_ADV;
    printf("[-] Start scanning!\n");
    gap_set_scan_parameters(1,0x0030, 0x0030);
    gap_start_scan(); 
}

// returns 1 if name is found in advertisement
static int advertisement_report_contains_name(const char * name, uint8_t * advertisement_report){
    // get advertisement from report event
    const uint8_t * adv_data = gap_event_advertising_report_get_data(advertisement_report);
    uint8_t         adv_len  = gap_event_advertising_report_get_data_length(advertisement_report);
    uint16_t        name_len = (uint8_t) strlen(name);

    // iterate over advertisement data
    ad_context_t context;
    for (ad_iterator_init(&context, adv_len, adv_data) ; ad_iterator_has_more(&context) ; ad_iterator_next(&context)){
        uint8_t data_type    = ad_iterator_get_data_type(&context);
        uint8_t data_size    = ad_iterator_get_data_len(&context);
        const uint8_t * data = ad_iterator_get_data(&context);
        switch (data_type){
            case BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME:
            case BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME:
                // compare prefix
                if (data_size < name_len) break;
                if (memcmp(data, name, name_len) == 0) return 1;
                return 1;
            default:
                break;
        }
    }
    return 0;
}

static void handle_gatt_client_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(packet_type);
    UNUSED(channel);
    UNUSED(size);

    uint8_t att_status;
    switch (app_state) {
        case APP_STATE_W4_SERVICE:
            switch(hci_event_packet_get_type(packet)){
                case GATT_EVENT_SERVICE_QUERY_RESULT:
                    // store service (we expect only one)
                    gatt_event_service_query_result_get_service(packet, &spark_40_service);
                    break;
                case GATT_EVENT_QUERY_COMPLETE:
                    att_status = gatt_event_query_complete_get_att_status(packet);
                    if (att_status != ATT_ERROR_SUCCESS){
                        printf("SERVICE_QUERY_RESULT - Error status %x.\n", att_status);
                        gap_disconnect(spark_40_connection_handle);
                        start_scanning();
                        break;
                    }
                    app_state = APP_STATE_W4_RX_CHARACTERISTIC;
                    printf("Search for Spark 40 RX characteristic.\n");
                    gatt_client_discover_characteristics_for_service_by_uuid16(handle_gatt_client_event,
               spark_40_connection_handle, &spark_40_service, spark_40_characteristic_rx_uuid);
                    break;
                default:
                    break;
            }
            break;
        case APP_STATE_W4_RX_CHARACTERISTIC:
            switch(hci_event_packet_get_type(packet)){
                case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT:
                    gatt_event_characteristic_query_result_get_characteristic(packet, &spark_40_characteristic_rx);
                    break;
                case GATT_EVENT_QUERY_COMPLETE:
                    att_status = gatt_event_query_complete_get_att_status(packet);
                    if (att_status != ATT_ERROR_SUCCESS){
                        printf("CHARACTERISTIC_QUERY_RESULT - Error status %x.\n", att_status);
                        gap_disconnect(spark_40_connection_handle);
                        start_scanning();
                        break;
                    }
                    app_state = APP_STATE_W4_TX_CHARACTERISTIC;
                    printf("Search for Spark 40 TX characteristic.\n");
                    gatt_client_discover_characteristics_for_service_by_uuid16(handle_gatt_client_event,
                               spark_40_connection_handle, &spark_40_service, spark_40_characteristic_tx_uuid);
                    break;
                default:
                    break;
            }
            break;
        case APP_STATE_W4_TX_CHARACTERISTIC:
            switch(hci_event_packet_get_type(packet)){
                case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT:
                    gatt_event_characteristic_query_result_get_characteristic(packet, &spark_40_characteristic_tx);
                    break;
                case GATT_EVENT_QUERY_COMPLETE:
                    att_status = gatt_event_query_complete_get_att_status(packet);
                    if (att_status != ATT_ERROR_SUCCESS){
                        printf("CHARACTERISTIC_QUERY_RESULT - Error status %x.\n", att_status);
                        gap_disconnect(spark_40_connection_handle);
                        start_scanning();
                        break;
                    }
                    printf("Subscribe for Spark 40 RX characteristic.\n");
                    // register handler for notifications
                    gatt_client_listen_for_characteristic_value_updates(&spark_40_notification_listener,
                        handle_gatt_client_event, spark_40_connection_handle, &spark_40_characteristic_rx);
                    app_state = APP_STATE_W4_RX_SUBSCRIBED;
                    gatt_client_write_client_characteristic_configuration(handle_gatt_client_event, spark_40_connection_handle,
                        &spark_40_characteristic_rx, GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
                    break;
                default:
                    break;
            }
            break;
        case APP_STATE_W4_RX_SUBSCRIBED:
            switch(hci_event_packet_get_type(packet)){
                case GATT_EVENT_QUERY_COMPLETE:
                    printf("Notifications enabled, ATT status %02x\n", gatt_event_query_complete_get_att_status(packet));
                    if (gatt_event_query_complete_get_att_status(packet) != ATT_ERROR_SUCCESS) break;
                    app_state = APP_STATE_CONNECTED;
                    select_preset(0);
                    break;
                default:
                    break;
            }
            break;
        case APP_STATE_CONNECTED:
            switch(hci_event_packet_get_type(packet)){
                case GATT_EVENT_NOTIFICATION:
                    process_update(gatt_event_notification_get_value(packet), gatt_event_notification_get_value_length(packet));
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }
}


static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            // BTstack activated, get started
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING){
                set_led(0x00, 0x00, 0xff);
                start_scanning();
            }
            break;
        case GAP_EVENT_ADVERTISING_REPORT:{
            // check name in advertisement
            if (!advertisement_report_contains_name(spark_40_device_name, packet)) return;
            // store address and type
            gap_event_advertising_report_get_address(packet, spark_40_addr);
            spark_40_addr_type = gap_event_advertising_report_get_address_type(packet);
            gap_stop_scan();
            printf("[-] Found Spark 40 - %s.\n", bd_addr_to_str(spark_40_addr));
            gap_connect(spark_40_addr,spark_40_addr_type);
            break;
        }
        case HCI_EVENT_LE_META:
            // wait for connection complete
            if (hci_event_le_meta_get_subevent_code(packet) != HCI_SUBEVENT_LE_CONNECTION_COMPLETE) break;
            spark_40_connection_handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
            printf("[-] Connection complete, discover services\n");

            // general gatt client request to trigger mandatory authentication
            app_state = APP_STATE_W4_SERVICE;
            gatt_client_discover_primary_services_by_uuid16(&handle_gatt_client_event, spark_40_connection_handle, spark_40_service_uuid);
            break;
        default:
            break;
    }
}

static void sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;

    bd_addr_t addr;
    bd_addr_type_t addr_type;

    switch (hci_event_packet_get_type(packet)) {
        case SM_EVENT_JUST_WORKS_REQUEST:
            printf("Just works requested\n");
            sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
            break;
        case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
            printf("Confirming numeric comparison: %"PRIu32"\n", sm_event_numeric_comparison_request_get_passkey(packet));
            sm_numeric_comparison_confirm(sm_event_passkey_display_number_get_handle(packet));
            break;
        case SM_EVENT_PAIRING_STARTED:
            printf("Pairing started\n");
            break;
        case SM_EVENT_PAIRING_COMPLETE:
            switch (sm_event_pairing_complete_get_status(packet)){
                case ERROR_CODE_SUCCESS:
                    printf("Pairing complete, success\n");
                    break;
                case ERROR_CODE_CONNECTION_TIMEOUT:
                    printf("Pairing failed, timeout\n");
                    break;
                case ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION:
                    printf("Pairing failed, disconnected\n");
                    break;
                case ERROR_CODE_AUTHENTICATION_FAILURE:
                    printf("Pairing failed, authentication failure with reason = %u\n", sm_event_pairing_complete_get_reason(packet));
                    break;
                default:
                    break;
            }
            break;
        case SM_EVENT_REENCRYPTION_STARTED:
            sm_event_reencryption_complete_get_address(packet, addr);
            printf("Bonding information exists for addr type %u, identity addr %s -> start re-encryption\n",
                   sm_event_reencryption_started_get_addr_type(packet), bd_addr_to_str(addr));
            break;
        case SM_EVENT_REENCRYPTION_COMPLETE:
            switch (sm_event_reencryption_complete_get_status(packet)){
                case ERROR_CODE_SUCCESS:
                    printf("Re-encryption complete, success\n");
                    break;
                case ERROR_CODE_CONNECTION_TIMEOUT:
                    printf("Re-encryption failed, timeout\n");
                    break;
                case ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION:
                    printf("Re-encryption failed, disconnected\n");
                    break;
                case ERROR_CODE_PIN_OR_KEY_MISSING:
                    printf("Re-encryption failed, bonding information missing\n\n");
                    printf("Assuming remote lost bonding information\n");
                    printf("Deleting local bonding information and start new pairing...\n");
                    sm_event_reencryption_complete_get_address(packet, addr);
                    addr_type = sm_event_reencryption_started_get_addr_type(packet);
                    gap_delete_bonding(addr_type, addr);
                    sm_request_pairing(sm_event_reencryption_complete_get_handle(packet));
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }
}

static void on_preset_updated(void){
    printf("[+] Preset: %u\n", spark_40_preset);
    switch (spark_40_preset){
        case 0: // clean
            set_led(0x00, 0xff, 0x00);
            break;
        case 1: // distortion
            set_led(0xff, 0x00, 0x00);
            break;
        default:
            break;
    }
}

// message format from
// https://github.com/jrnelson90/tinderboxpedal/blob/master/src/BLE%20message%20format.md

static void process_update(const uint8_t * data, uint16_t len){

#ifdef LOG_MESSAGES
    printf("RX: ");
    printf_hexdump(data, len);
#endif

    // check for preset change
    if (len < 7){
        return;
    }
    if ((data[6] == 0x1A) && (len == 0x1a)){
        if ((data[20] == 0x03) && data[21] == 0x38){
            spark_40_preset = data[24];
            on_preset_updated();
        }
    }
}

static void send_command(const uint8_t * command, uint16_t command_len){
    uint8_t message[100];
    uint8_t prefix[] = { 0x01, 0xFE, 0x00, 0x00, 0x53, 0xFE };
    uint8_t middle[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0, 0x00, 0xf0, 0x01, 0x01, 0x01 };
    uint8_t suffix[] = { 0xf7 };
    uint16_t len = sizeof(prefix) + 1 + sizeof(middle) + command_len + sizeof(suffix);
    uint16_t pos = 0;
    memcpy(&message[pos], prefix, sizeof(prefix));
    pos += sizeof(prefix);
    message[pos++] = len;
    memcpy(&message[pos], middle, sizeof(middle));
    pos += sizeof(middle);
    memcpy(&message[pos], command, command_len);
    pos += command_len;
    message[pos++] = 0x0f7;

#ifdef LOG_MESSAGES
    printf("TX: ");
    printf_hexdump(message, pos);
#endif

    gatt_client_write_value_of_characteristic(handle_gatt_client_event, spark_40_connection_handle, spark_40_characteristic_tx.value_handle, len, message);
}

static void select_preset(uint8_t preset){
    if (app_state != APP_STATE_CONNECTED){
        return;
    }

    uint8_t tone[]   = {0x01, 0x38, 0x00, 0x00, 0x00};
    spark_40_preset = preset;
    tone[4] = preset;
    send_command(tone, sizeof(tone));
    on_preset_updated();
}

static void stdin_handler(char c){
    static uint8_t config[] = {0x02, 0x01, 0x00, 0x00, 0x00};
    static uint8_t get_hw_id[] = { 0x02, 0x23 };
    switch (c){
        case '1':
        case '2':
        case '3':
        case '4':
            select_preset(c - '1');
            break;
        case '5':
        case '6':
        case '7':
        case '8':
            config[4] = c - '5';
            send_command(config, sizeof(config));
            break;
        case '9':
            send_command(get_hw_id, sizeof(get_hw_id));
            break;
        default:
            break;
    }
}

int btstack_main(void);
int btstack_main(void)
{
    platform_init();

    l2cap_init();

    // setup SM: Display only
    sm_init();

    // setup GATT Client
    gatt_client_init();

    // register handler
    hci_event_callback_registration.callback = &hci_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    sm_event_callback_registration.callback = &sm_packet_handler;
    sm_add_event_handler(&sm_event_callback_registration);

    btstack_stdin_setup(&stdin_handler);

    // turn on!
    hci_power_control(HCI_POWER_ON);
        
    return 0;
}

/* EXAMPLE_END */
