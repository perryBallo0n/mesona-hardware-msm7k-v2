/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


// #define LOG_NDEBUG 0
#define LOG_TAG "lights"

#include <cutils/log.h>
#include <cutils/properties.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>

#include <sys/ioctl.h>
#include <sys/types.h>

#include <hardware/lights.h>

/******************************************************************************/

static pthread_once_t g_init = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_haveTrackballLight = 0;
static struct light_state_t g_notification;
static struct light_state_t g_battery;
static int g_backlight = 255;
static int g_trackball = -1;
static int g_buttons = 0;
static int g_attention = 0;
static int g_haveAmberLed = 0;

char const*const TRACKBALL_FILE
        = "/sys/class/leds/jogball-backlight/brightness";

char const*const RED_LED_FILE
        = "/sys/class/leds/red/brightness";

char const*const GREEN_LED_FILE
        = "/sys/class/leds/green/brightness";

char const*const BLUE_LED_FILE
        = "/sys/class/leds/blue/brightness";

char const*const AMBER_LED_FILE
        = "/sys/class/leds/amber/brightness";

char const*const LCD_FILE
        = "/sys/class/leds/lcd-backlight/brightness";

char const*const RED_FREQ_FILE
        = "/sys/class/leds/red/device/grpfreq";

char const*const RED_PWM_FILE
        = "/sys/class/leds/red/device/grppwm";

char const*const RED_BLINK_FILE
        = "/sys/class/leds/red/device/blink";

char const*const AMBER_BLINK_FILE
        = "/sys/class/leds/amber/blink";

char const*const KEYBOARD_FILE
        = "/sys/class/leds/keyboard-backlight/brightness";

char const*const BUTTON_FILE
        = "/sys/class/leds/button-backlight/brightness";

/**
 * device methods
 */

void init_globals(void)
{
    // init the mutex
    pthread_mutex_init(&g_lock, NULL);

    // figure out if we have the trackball LED or not
    g_haveTrackballLight = (access(TRACKBALL_FILE, W_OK) == 0) ? 1 : 0;

    /* figure out if we have the amber LED or not.
       If yes, just support green and amber.         */
    g_haveAmberLed = (access(AMBER_LED_FILE, W_OK) == 0) ? 1 : 0;
}

static int
write_int(char const* path, int value)
{
    int fd;
    static int already_warned = 0;

    fd = open(path, O_RDWR);
    if (fd >= 0) {
        char buffer[20];
        int bytes = sprintf(buffer, "%d\n", value);
        int amt = write(fd, buffer, bytes);
        close(fd);
        return amt == -1 ? -errno : 0;
    } else {
        if (already_warned == 0) {
            LOGE("write_int failed to open %s\n", path);
            already_warned = 1;
        }
        return -errno;
    }
}

static int
is_lit(struct light_state_t const* state)
{
    return state->color & 0x00ffffff;
}

static int
handle_trackball_light_locked(struct light_device_t* dev)
{
    int mode = g_attention;

    if (mode == 7 && g_backlight) {
        mode = 0;
    }
    LOGV("%s g_backlight = %d, mode = %d, g_attention = %d\n",
        __func__, g_backlight, mode, g_attention);

    // If the value isn't changing, don't set it, because this
    // can reset the timer on the breathing mode, which looks bad.
    if (g_trackball == mode) {
        return 0;
    }

    return write_int(TRACKBALL_FILE, mode);
}

static int
rgb_to_brightness(struct light_state_t const* state)
{
    int color = state->color & 0x00ffffff;
    return ((77*((color>>16)&0x00ff))
            + (150*((color>>8)&0x00ff)) + (29*(color&0x00ff))) >> 8;
}

static int
set_light_backlight(struct light_device_t* dev,
        struct light_state_t const* state)
{
    int err = 0;
    int brightness = rgb_to_brightness(state);
    pthread_mutex_lock(&g_lock);
    g_backlight = brightness;
    err = write_int(LCD_FILE, brightness);
    if (g_haveTrackballLight) {
        handle_trackball_light_locked(dev);
    }
    pthread_mutex_unlock(&g_lock);
    return err;
}

static int
set_light_keyboard(struct light_device_t* dev,
        struct light_state_t const* state)
{
    int err = 0;
    int on = is_lit(state);
    pthread_mutex_lock(&g_lock);
    err = write_int(KEYBOARD_FILE, on?255:0);
    pthread_mutex_unlock(&g_lock);
    return err;
}

static int
set_light_buttons(struct light_device_t* dev,
        struct light_state_t const* state)
{
    int err = 0;
    int on = is_lit(state);
    pthread_mutex_lock(&g_lock);
    g_buttons = on;
    err = write_int(BUTTON_FILE, on?255:0);
    pthread_mutex_unlock(&g_lock);
    return err;
}


static void write_fih_led(int id, int val)
{
    int fd = open("/sys/class/led/fih_led/control", O_WRONLY);
    if (fd >= 0) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "1 %d %d\n", id, val ? 1 : 0);
        write(fd, buf, len);
        close(fd);
    }
}

static void
write_fih_sub_bar(int red, int green, int blue, int scale_max, int fade, int interval_ms)
{
    char busy[PROPERTY_VALUE_MAX] = {0};
    property_get("sys.fih_led.busy", busy, "0");
    if (busy[0] == '1')
        return;

    int pwms[3];
    int raw[3] = { red, green, blue };
    int i;
    int any_lit = 0;

    if (interval_ms < 10) interval_ms = 10;
    for (i = 0; i < 3; i++) {
        if (raw[i] > 0 && scale_max > 0) {
            int p = (raw[i] * scale_max + 127) / 255;
            if (p < 12) p = 12;
            if (p > 235) p = 235;
            pwms[i] = p;
            any_lit = 1;
        } else {
            pwms[i] = 0;
        }
    }

    int fd = open("/sys/class/led/fih_led/control", O_WRONLY);
    if (fd >= 0) {
        char buf[384];
        int len = 0;
        const int ids[3] = { 4, 5, 6 };

        len += snprintf(buf + len, sizeof(buf) - len,
                        "3 4 0\n3 5 0\n3 6 0\n1 4 0\n1 5 0\n1 6 0\n");

        if (any_lit && fade) {
            for (i = 0; i < 3 && len < (int)sizeof(buf) - 64; i++) {
                if (pwms[i] > 0) {
                    len += snprintf(buf + len, sizeof(buf) - len,
                                    "16 %d 1 1 1\n6 %d %d\n25 %d %d\n",
                                    ids[i], ids[i], interval_ms, ids[i], pwms[i]);
                }
            }
            for (i = 0; i < 3 && len < (int)sizeof(buf) - 16; i++) {
                if (pwms[i] > 0) {
                    len += snprintf(buf + len, sizeof(buf) - len, "3 %d 1\n", ids[i]);
                }
            }
        } else if (any_lit) {
            for (i = 0; i < 3 && len < (int)sizeof(buf) - 32; i++) {
                if (pwms[i] > 0) {
                    len += snprintf(buf + len, sizeof(buf) - len,
                                    "4 %d %d\n1 %d 1\n", ids[i], pwms[i], ids[i]);
                }
            }
        }

        if (len > 0) {
            write(fd, buf, len);
        }
        close(fd);
    }
}

static int
set_speaker_light_locked(struct light_device_t* dev,
        struct light_state_t const* state)
{
    int len;
    int alpha, red, green, blue;
    int blink, freq, pwm;
    int onMS, offMS;
    unsigned int colorRGB;

    switch (state->flashMode) {
        case LIGHT_FLASH_TIMED:
            onMS = state->flashOnMS;
            offMS = state->flashOffMS;
            break;
        case LIGHT_FLASH_NONE:
        default:
            onMS = 0;
            offMS = 0;
            break;
    }

    colorRGB = state->color;

#if 0
    LOGD("set_speaker_light_locked colorRGB=%08X, onMS=%d, offMS=%d\n",
            colorRGB, onMS, offMS);
#endif

    red = (colorRGB >> 16) & 0xFF;
    green = (colorRGB >> 8) & 0xFF;
    blue = colorRGB & 0xFF;

    if (!g_haveAmberLed) {
        write_int(RED_LED_FILE, red);
        write_int(GREEN_LED_FILE, green);
        write_int(BLUE_LED_FILE, blue);
        // FIH Miro front RGB notification LED
        write_fih_led(0, red > 0 ? 1 : 0);
        write_fih_led(1, green > 0 ? 1 : 0);
        write_fih_led(2, blue > 0 ? 1 : 0);
        // Accent bottom chin RGB bar (channels 4,5,6) governed by CMParts properties
        {
            char prop_val[PROPERTY_VALUE_MAX];
            int master_on = 1;
            int notif_on = 1;
            int chg_on = 0;
            int bright = 100;
            if (property_get("persist.sys.bottom_led_enabled", prop_val, "1") > 0 && prop_val[0] == '0')
                master_on = 0;
            if (property_get("persist.sys.bottom_led_notif", prop_val, "1") > 0 && prop_val[0] == '0')
                notif_on = 0;
            if (property_get("persist.sys.bottom_led_charging", prop_val, "0") > 0 && prop_val[0] != '0')
                chg_on = 1;
            if (property_get("persist.sys.bottom_led_bright", prop_val, "100") > 0) {
                bright = atoi(prop_val);
                if (bright < 25) bright = 25;
                if (bright > 150) bright = 150;
            }
            if (master_on && is_lit(&g_notification) && notif_on) {
                /* Nokia 3710 Fold-style warm yellow-orange breathing glow for unread notifications */
                int scale_max = (185 * bright) / 100;
                if (scale_max > 235) scale_max = 235;
                write_fih_sub_bar(255, 125, 0, scale_max, 1, 52);
            } else if (master_on && !is_lit(&g_notification) && is_lit(&g_battery) && chg_on && (red > 0 || green > 0 || blue > 0)) {
                int scale_max = (90 * bright) / 100;
                if (scale_max > 225) scale_max = 225;
                write_fih_sub_bar(red, green, blue, scale_max, 0, 32);
            } else {
                write_fih_sub_bar(0, 0, 0, 0, 0, 32);
            }
        }
    } else {
        /* all of related red led is replaced by amber */
        if (red) {
            write_int(AMBER_LED_FILE, 1);
            write_int(GREEN_LED_FILE, 0);
        } else if (green) {
            write_int(AMBER_LED_FILE, 0);
            write_int(GREEN_LED_FILE, 1);
        } else {
            write_int(GREEN_LED_FILE, 0);
            write_int(AMBER_LED_FILE, 0);
        }
    }

    if (onMS > 0 && offMS > 0) {
        int totalMS = onMS + offMS;

        // the LED appears to blink about once per second if freq is 20
        // 1000ms / 20 = 50
        freq = totalMS / 50;
        // pwm specifies the ratio of ON versus OFF
        // pwm = 0 -> always off
        // pwm = 255 => always on
        pwm = (onMS * 255) / totalMS;

        // the low 4 bits are ignored, so round up if necessary
        if (pwm > 0 && pwm < 16)
            pwm = 16;

        blink = 1;
    } else {
        blink = 0;
        freq = 0;
        pwm = 0;
    }

    if (!g_haveAmberLed) {
        if (blink) {
            write_int(RED_FREQ_FILE, freq);
            write_int(RED_PWM_FILE, pwm);
        }
        write_int(RED_BLINK_FILE, blink);
    } else {
        write_int(AMBER_BLINK_FILE, blink);
    }

    return 0;
}

static void
handle_speaker_battery_locked(struct light_device_t* dev)
{
    if (is_lit(&g_battery)) {
        set_speaker_light_locked(dev, &g_battery);
    } else {
        set_speaker_light_locked(dev, &g_notification);
    }
}

static int
set_light_battery(struct light_device_t* dev,
        struct light_state_t const* state)
{
    pthread_mutex_lock(&g_lock);
    g_battery = *state;
    if (g_haveTrackballLight) {
        set_speaker_light_locked(dev, state);
    }
    handle_speaker_battery_locked(dev);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static int
set_light_notifications(struct light_device_t* dev,
        struct light_state_t const* state)
{
    pthread_mutex_lock(&g_lock);
    g_notification = *state;
    LOGV("set_light_notifications g_trackball=%d color=0x%08x",
            g_trackball, state->color);
    if (g_haveTrackballLight) {
        handle_trackball_light_locked(dev);
    }
    handle_speaker_battery_locked(dev);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static int
set_light_attention(struct light_device_t* dev,
        struct light_state_t const* state)
{
    pthread_mutex_lock(&g_lock);
    LOGV("set_light_attention g_trackball=%d color=0x%08x",
            g_trackball, state->color);
    if (state->flashMode == LIGHT_FLASH_HARDWARE) {
        g_attention = state->flashOnMS;
    } else if (state->flashMode == LIGHT_FLASH_NONE) {
        g_attention = 0;
    }
    if (g_haveTrackballLight) {
        handle_trackball_light_locked(dev);
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}


/** Close the lights device */
static int
close_lights(struct light_device_t *dev)
{
    if (dev) {
        free(dev);
    }
    return 0;
}


/******************************************************************************/

/**
 * module methods
 */

/** Open a new instance of a lights device using name */
static int open_lights(const struct hw_module_t* module, char const* name,
        struct hw_device_t** device)
{
    int (*set_light)(struct light_device_t* dev,
            struct light_state_t const* state);

    if (0 == strcmp(LIGHT_ID_BACKLIGHT, name)) {
        set_light = set_light_backlight;
    }
    else if (0 == strcmp(LIGHT_ID_KEYBOARD, name)) {
        set_light = set_light_keyboard;
    }
    else if (0 == strcmp(LIGHT_ID_BUTTONS, name)) {
        set_light = set_light_buttons;
    }
    else if (0 == strcmp(LIGHT_ID_BATTERY, name)) {
        set_light = set_light_battery;
    }
    else if (0 == strcmp(LIGHT_ID_NOTIFICATIONS, name)) {
        set_light = set_light_notifications;
    }
    else if (0 == strcmp(LIGHT_ID_ATTENTION, name)) {
        set_light = set_light_attention;
    }
    else {
        return -EINVAL;
    }

    pthread_once(&g_init, init_globals);

    struct light_device_t *dev = malloc(sizeof(struct light_device_t));
    memset(dev, 0, sizeof(*dev));

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = (struct hw_module_t*)module;
    dev->common.close = (int (*)(struct hw_device_t*))close_lights;
    dev->set_light = set_light;

    *device = (struct hw_device_t*)dev;
    return 0;
}


static struct hw_module_methods_t lights_module_methods = {
    .open =  open_lights,
};

/*
 * The lights Module
 */
const struct hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .version_major = 1,
    .version_minor = 0,
    .id = LIGHTS_HARDWARE_MODULE_ID,
    .name = "QCT MSM7K lights Module",
    .author = "Google, Inc.",
    .methods = &lights_module_methods,
};
