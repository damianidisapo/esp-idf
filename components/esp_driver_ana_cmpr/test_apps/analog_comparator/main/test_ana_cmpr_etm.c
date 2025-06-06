/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <inttypes.h>
#include "esp_attr.h"
#include "esp_etm.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "driver/gptimer_etm.h"
#include "test_ana_cmpr.h"

#define TEST_TIME_US        5000

static gptimer_handle_t test_ana_cmpr_gptimer_init(void)
{
    gptimer_handle_t gptimer = NULL;
    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1 * 1000 * 1000, // 1MHz, 1 tick = 1us
    };
    TEST_ESP_OK(gptimer_new_timer(&timer_config, &gptimer));
    TEST_ESP_OK(gptimer_set_raw_count(gptimer, 0));
    TEST_ESP_OK(gptimer_enable(gptimer));
    return gptimer;
}

static void test_ana_cmpr_gptimer_deinit(gptimer_handle_t gptimer)
{
    TEST_ESP_OK(gptimer_disable(gptimer));
    TEST_ESP_OK(gptimer_del_timer(gptimer));
}

static ana_cmpr_handle_t test_ana_cmpr_init(void)
{
    ana_cmpr_handle_t cmpr = NULL;

    ana_cmpr_config_t config = {
        .unit = TEST_ANA_CMPR_UNIT_ID,
        .clk_src = ANA_CMPR_CLK_SRC_DEFAULT,
        .ref_src = ANA_CMPR_REF_SRC_INTERNAL,
        .cross_type = ANA_CMPR_CROSS_ANY,
    };
    TEST_ESP_OK(ana_cmpr_new_unit(&config, &cmpr));

    ana_cmpr_internal_ref_config_t ref_cfg = {
        .ref_volt = ANA_CMPR_REF_VOLT_50_PCT_VDD,
    };
    TEST_ESP_OK(ana_cmpr_set_internal_reference(cmpr, &ref_cfg));

    ana_cmpr_debounce_config_t dbc_cfg = {
        .wait_us = 10,
    };
    TEST_ESP_OK(ana_cmpr_set_debounce(cmpr, &dbc_cfg));
    TEST_ESP_OK(ana_cmpr_enable(cmpr));

    return cmpr;
}

static void test_ana_cmpr_deinit(ana_cmpr_handle_t cmpr)
{
    TEST_ESP_OK(ana_cmpr_disable(cmpr));
    TEST_ESP_OK(ana_cmpr_del_unit(cmpr));
}

typedef struct {
    esp_etm_event_handle_t cmpr_pos_evt;
    esp_etm_event_handle_t cmpr_neg_evt;
    esp_etm_task_handle_t gptimer_start_task;
    esp_etm_task_handle_t gptimer_stop_task;
    esp_etm_channel_handle_t etm_pos_handle;
    esp_etm_channel_handle_t etm_neg_handle;
} test_ana_cmpr_etm_handles_t;

static test_ana_cmpr_etm_handles_t test_ana_cmpr_init_etm(ana_cmpr_handle_t cmpr, gptimer_handle_t gptimer)
{
    test_ana_cmpr_etm_handles_t etm_handles = {};

    /* Allocate the analog comparator positive & negative cross events */
    ana_cmpr_etm_event_config_t evt_cfg = {
        .event_type = ANA_CMPR_EVENT_POS_CROSS,
    };
    TEST_ESP_OK(ana_cmpr_new_etm_event(cmpr, &evt_cfg, &etm_handles.cmpr_pos_evt));
    evt_cfg.event_type = ANA_CMPR_EVENT_NEG_CROSS;
    TEST_ESP_OK(ana_cmpr_new_etm_event(cmpr, &evt_cfg, &etm_handles.cmpr_neg_evt));

    /* Allocate the GPTimer tasks */
    gptimer_etm_task_config_t gptimer_etm_task_conf = {
        .task_type = GPTIMER_ETM_TASK_START_COUNT,
    };
    TEST_ESP_OK(gptimer_new_etm_task(gptimer, &gptimer_etm_task_conf, &etm_handles.gptimer_start_task));
    gptimer_etm_task_conf.task_type = GPTIMER_ETM_TASK_STOP_COUNT;
    TEST_ESP_OK(gptimer_new_etm_task(gptimer, &gptimer_etm_task_conf, &etm_handles.gptimer_stop_task));

    /* Allocate the Event Task Matrix channels */
    esp_etm_channel_config_t etm_cfg = {};
    TEST_ESP_OK(esp_etm_new_channel(&etm_cfg, &etm_handles.etm_pos_handle));
    TEST_ESP_OK(esp_etm_new_channel(&etm_cfg, &etm_handles.etm_neg_handle));
    /* Bind the events and tasks */
    TEST_ESP_OK(esp_etm_channel_connect(etm_handles.etm_neg_handle, etm_handles.cmpr_neg_evt, etm_handles.gptimer_start_task));
    TEST_ESP_OK(esp_etm_channel_connect(etm_handles.etm_pos_handle, etm_handles.cmpr_pos_evt, etm_handles.gptimer_stop_task));
    /* Enable the ETM channels */
    TEST_ESP_OK(esp_etm_channel_enable(etm_handles.etm_pos_handle));
    TEST_ESP_OK(esp_etm_channel_enable(etm_handles.etm_neg_handle));

    return etm_handles;
}

static void test_ana_cmpr_deinit_etm(test_ana_cmpr_etm_handles_t handles)
{
    TEST_ESP_OK(esp_etm_channel_disable(handles.etm_pos_handle));
    TEST_ESP_OK(esp_etm_channel_disable(handles.etm_neg_handle));

    TEST_ESP_OK(esp_etm_del_task(handles.gptimer_start_task));
    TEST_ESP_OK(esp_etm_del_task(handles.gptimer_stop_task));

    TEST_ESP_OK(esp_etm_del_event(handles.cmpr_pos_evt));
    TEST_ESP_OK(esp_etm_del_event(handles.cmpr_neg_evt));

    TEST_ESP_OK(esp_etm_del_channel(handles.etm_pos_handle));
    TEST_ESP_OK(esp_etm_del_channel(handles.etm_neg_handle));
}

TEST_CASE("ana_cmpr etm event", "[ana_cmpr][etm]")
{
    gptimer_handle_t gptimer = test_ana_cmpr_gptimer_init();
    ana_cmpr_handle_t cmpr = test_ana_cmpr_init();
    int src_gpio = test_init_src_chan_gpio(TEST_ANA_CMPR_UNIT_ID, 1);
    test_ana_cmpr_etm_handles_t handles = test_ana_cmpr_init_etm(cmpr, gptimer);

    // triggers a negative pulse, whose duration is ~TEST_TIME_US
    // negedge triggers the gptimer to start task
    // posedge triggers the gptimer to stop task
    // gptimer will record the time between the negedge and posedge
    gpio_set_level(src_gpio, 0);
    esp_rom_delay_us(TEST_TIME_US);
    gpio_set_level(src_gpio, 1);

    uint64_t cnt_us = 0;
    TEST_ESP_OK(gptimer_get_raw_count(gptimer, &cnt_us));
    printf("Count: %" PRIu64 "\n", cnt_us);
    // gptimer timer should stopped
    uint64_t cnt_us_again = 0;
    TEST_ESP_OK(gptimer_get_raw_count(gptimer, &cnt_us_again));
    TEST_ASSERT(cnt_us_again == cnt_us);

    test_ana_cmpr_deinit_etm(handles);
    test_ana_cmpr_deinit(cmpr);
    test_ana_cmpr_gptimer_deinit(gptimer);

    TEST_ASSERT_UINT_WITHIN(TEST_TIME_US * 0.1, TEST_TIME_US, cnt_us);
}
