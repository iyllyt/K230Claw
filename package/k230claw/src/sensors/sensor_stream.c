/*
 * sensor_stream.c - 流媒体传感器包装器
 *
 * 包装 hal_camera_stream start/stop 为 kc_sensor_t，
 * 供角色系统按需启停。
 */

#include "sensor_mgr.h"
#include "../hal/hal_camera_stream.h"

static kc_err_t stream_sensor_start(kc_sensor_t *self)
{
    (void)self;
    return hal_camera_stream_start();
}

static kc_err_t stream_sensor_stop(kc_sensor_t *self)
{
    (void)self;
    hal_camera_stream_stop();
    return KC_OK;
}

static kc_sensor_t s_stream_sensor = {
    .name = "video_stream",
    .start = stream_sensor_start,
    .stop  = stream_sensor_stop,
    .ctx   = NULL
};

kc_sensor_t *video_stream_get(void)
{
    return &s_stream_sensor;
}
