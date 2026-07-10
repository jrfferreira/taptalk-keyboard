/* Application state machine — pure transition function.
 *
 * sm_step() has no side effects: it maps (state, event, guards) to a next
 * state plus a bitmask of actions for the caller to perform. That split is
 * what lets every transition be exercised on the host, with no FreeRTOS, no
 * radio, and no board. app_sm.c is the impure half that runs the actions. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ST_BOOT,
    ST_PMIC_INIT,
    ST_PROVISIONING, /* SoftAP + captive portal; no credentials yet */
    ST_WIFI_CONNECTING,
    ST_TIME_SYNC,
    ST_IDLE_READY,
    ST_RECORDING,
    ST_UPLOADING,
    ST_TYPING,
    ST_NOT_READY, /* was ready, lost a precondition */
    ST_ERROR,
    ST_COUNT,
} app_state_t;

typedef enum {
    EV_NONE,
    EV_PMIC_OK,
    EV_PMIC_FAIL,
    EV_WIFI_UP,
    EV_WIFI_DOWN,
    EV_TIME_OK,
    EV_TIME_FAIL,
    EV_BTN_PRESS,
    EV_BTN_RELEASE,
    EV_PRESS_LOST, /* finger slid off the button */
    EV_REC_MAX,    /* hit the duration cap, finger may still be down */
    EV_STT_OK,
    EV_STT_EMPTY,
    EV_STT_FAIL,
    EV_TYPE_DONE,
    EV_TYPE_ABORT,
    EV_USB_MOUNT,
    EV_USB_UNMOUNT,
    EV_RETRY,
    EV_TIMEOUT,
    EV_ENTER_SETUP, /* user asked for the portal, or Wi-Fi never worked */
    EV_PROVISIONED, /* credentials accepted and written to NVS */
    EV_SETUP_EXIT,  /* backed out of setup via the Back button; reboot to normal */
    EV_COUNT,
} app_event_t;

typedef struct {
    bool provisioned; /* NVS holds an SSID */
    bool wifi_up;
    bool time_ok;
    bool usb_mounted;
    bool clip_usable; /* long enough, and not silence */
    uint32_t wifi_retries;
} sm_guards_t;

#define SM_WIFI_MAX_RETRIES 5

enum {
    ACT_NONE           = 0,
    ACT_PMIC_INIT      = 1u << 0,
    ACT_WIFI_START     = 1u << 1,
    ACT_WIFI_RETRY     = 1u << 2,
    ACT_SNTP_START     = 1u << 3,
    ACT_REC_START      = 1u << 4,
    ACT_REC_STOP       = 1u << 5,
    ACT_CLIP_DISCARD   = 1u << 6,
    ACT_UPLOAD_START   = 1u << 7,
    ACT_UPLOAD_ABORT   = 1u << 8,
    ACT_TYPE_START     = 1u << 9,
    ACT_TYPE_ABORT     = 1u << 10,
    ACT_SHOW_ERROR     = 1u << 11,
    ACT_HINT_NOT_READY = 1u << 12,
    ACT_PROV_START     = 1u << 13,
    ACT_REBOOT         = 1u << 14,
    ACT_HINT_QUIET     = 1u << 15, /* clip dropped: tell the user why */
};

typedef struct {
    app_state_t next;
    uint32_t actions;
} sm_out_t;

/* Unhandled (state, event) pairs are a no-op: next == current, actions == 0.
 * Events genuinely arrive out of order (a release after we already gave up on
 * the finger, a mount during upload), so silently ignoring them is correct —
 * not a latent bug being swallowed. */
sm_out_t sm_step(app_state_t state, app_event_t event, const sm_guards_t *guards);

const char *sm_state_name(app_state_t state);

/* A transition logged as "IDLE_READY --10--> UPLOADING" costs a trip to this
 * header to decode. Named, it reads "IDLE_READY --REC_MAX--> UPLOADING", and
 * the fact that a 30-second cap fired rather than a finger lifting is right
 * there in the line. */
const char *sm_event_name(app_event_t event);
