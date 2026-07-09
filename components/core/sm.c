#include "core/sm.h"

static sm_out_t out(app_state_t next, uint32_t actions)
{
    sm_out_t o = {.next = next, .actions = actions};
    return o;
}

static bool ready_to_record(const sm_guards_t *g)
{
    return g->wifi_up && g->time_ok && g->usb_mounted;
}

sm_out_t sm_step(app_state_t state, app_event_t event, const sm_guards_t *g)
{
    const sm_out_t ignore = out(state, ACT_NONE);

    switch (state) {

    case ST_BOOT:
        /* Any event other than a deliberate start is noise this early. */
        return out(ST_PMIC_INIT, ACT_PMIC_INIT);

    case ST_PMIC_INIT:
        if (event == EV_PMIC_OK)   return out(ST_WIFI_CONNECTING, ACT_WIFI_START);
        if (event == EV_PMIC_FAIL) return out(ST_ERROR, ACT_SHOW_ERROR);
        return ignore;

    case ST_WIFI_CONNECTING:
        if (event == EV_WIFI_UP) return out(ST_TIME_SYNC, ACT_SNTP_START);
        /* A failed association surfaces as a disconnect, not a timeout, so
         * both must drive the retry ladder or we sit here forever. */
        if (event == EV_TIMEOUT || event == EV_WIFI_DOWN) {
            return g->wifi_retries < SM_WIFI_MAX_RETRIES
                       ? out(ST_WIFI_CONNECTING, ACT_WIFI_RETRY)
                       : out(ST_ERROR, ACT_SHOW_ERROR);
        }
        return ignore;

    case ST_TIME_SYNC:
        /* Without a synced clock, TLS certificate validation fails against
         * every backend. There is no useful degraded mode. */
        if (event == EV_TIME_OK)   return out(ST_IDLE_READY, ACT_NONE);
        if (event == EV_TIME_FAIL) return out(ST_ERROR, ACT_SHOW_ERROR);
        if (event == EV_WIFI_DOWN) return out(ST_WIFI_CONNECTING, ACT_WIFI_RETRY);
        return ignore;

    case ST_IDLE_READY:
        if (event == EV_BTN_PRESS) {
            return ready_to_record(g) ? out(ST_RECORDING, ACT_REC_START)
                                      : out(ST_IDLE_READY, ACT_HINT_NOT_READY);
        }
        if (event == EV_WIFI_DOWN || event == EV_USB_UNMOUNT) {
            return out(ST_NOT_READY, ACT_NONE);
        }
        return ignore;

    case ST_NOT_READY:
        if (event == EV_WIFI_UP || event == EV_USB_MOUNT) {
            /* Both preconditions must hold; the guards carry the other one. */
            return ready_to_record(g) ? out(ST_IDLE_READY, ACT_NONE) : ignore;
        }
        if (event == EV_BTN_PRESS) return out(ST_NOT_READY, ACT_HINT_NOT_READY);
        return ignore;

    case ST_RECORDING:
        /* PRESS_LOST is treated exactly like a release. A finger sliding off
         * the button almost always means "I finished talking"; discarding
         * would silently eat the sentence, which is the worse failure. The
         * clip_usable guard still filters out taps and silence. */
        if (event == EV_BTN_RELEASE || event == EV_PRESS_LOST) {
            return g->clip_usable ? out(ST_UPLOADING, ACT_REC_STOP | ACT_UPLOAD_START)
                                  : out(ST_IDLE_READY, ACT_REC_STOP | ACT_CLIP_DISCARD);
        }
        /* Hitting the cap uploads immediately rather than waiting for the
         * finger, so there is no "recording but not really" limbo. The later
         * release lands in ST_UPLOADING and is ignored there. */
        if (event == EV_REC_MAX) return out(ST_UPLOADING, ACT_REC_STOP | ACT_UPLOAD_START);
        if (event == EV_WIFI_DOWN || event == EV_USB_UNMOUNT) {
            return out(ST_NOT_READY, ACT_REC_STOP | ACT_CLIP_DISCARD);
        }
        return ignore;

    case ST_UPLOADING:
        if (event == EV_STT_OK)    return out(ST_TYPING, ACT_TYPE_START);
        if (event == EV_STT_EMPTY) return out(ST_IDLE_READY, ACT_CLIP_DISCARD);
        if (event == EV_STT_FAIL)  return out(ST_ERROR, ACT_SHOW_ERROR);
        if (event == EV_WIFI_DOWN || event == EV_USB_UNMOUNT) {
            return out(ST_ERROR, ACT_UPLOAD_ABORT | ACT_SHOW_ERROR);
        }
        /* The release that ended the recording, or one swallowed after
         * EV_REC_MAX. Either way the clip is already in flight. */
        if (event == EV_BTN_RELEASE || event == EV_PRESS_LOST) return ignore;
        return ignore;

    case ST_TYPING:
        if (event == EV_TYPE_DONE)  return out(ST_IDLE_READY, ACT_CLIP_DISCARD);
        if (event == EV_TYPE_ABORT) return out(ST_ERROR, ACT_SHOW_ERROR);
        if (event == EV_USB_UNMOUNT) return out(ST_ERROR, ACT_TYPE_ABORT | ACT_SHOW_ERROR);
        return ignore;

    case ST_ERROR:
        if (event == EV_RETRY) {
            return g->wifi_up ? out(ST_IDLE_READY, ACT_CLIP_DISCARD)
                              : out(ST_WIFI_CONNECTING, ACT_WIFI_RETRY);
        }
        if (event == EV_TIMEOUT) return out(ST_IDLE_READY, ACT_CLIP_DISCARD);
        return ignore;

    case ST_COUNT:
    default:
        return ignore;
    }
}

const char *sm_state_name(app_state_t state)
{
    switch (state) {
    case ST_BOOT:            return "BOOT";
    case ST_PMIC_INIT:       return "PMIC_INIT";
    case ST_WIFI_CONNECTING: return "WIFI_CONNECTING";
    case ST_TIME_SYNC:       return "TIME_SYNC";
    case ST_IDLE_READY:      return "IDLE_READY";
    case ST_RECORDING:       return "RECORDING";
    case ST_UPLOADING:       return "UPLOADING";
    case ST_TYPING:          return "TYPING";
    case ST_NOT_READY:       return "NOT_READY";
    case ST_ERROR:           return "ERROR";
    case ST_COUNT:
    default:                 return "?";
    }
}
