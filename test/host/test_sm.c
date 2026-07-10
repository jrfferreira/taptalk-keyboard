#include "core/sm.h"
#include "test_util.h"

static sm_guards_t all_ready(void)
{
    sm_guards_t g = {
        .provisioned = true, .wifi_up = true, .time_ok = true, .time_required = true,
        .usb_mounted = true,
        .clip_usable = true, .has_pending = true, .wifi_retries = 0,
    };
    return g;
}

/* Addressable form, for the one-liner call sites below. */
static const sm_guards_t *ready(void)
{
    static sm_guards_t g;
    g = all_ready();
    return &g;
}

TEST_MAIN("sm", {
    sm_guards_t g;
    sm_out_t o;

    /* --- boot path --- */
    g = all_ready();
    o = sm_step(ST_BOOT, EV_NONE, &g);
    CHECK_EQ_INT(o.next, ST_PMIC_INIT);
    CHECK(o.actions & ACT_PMIC_INIT);

    o = sm_step(ST_PMIC_INIT, EV_PMIC_OK, &g);
    CHECK_EQ_INT(o.next, ST_WIFI_CONNECTING);
    CHECK(o.actions & ACT_WIFI_START);

    o = sm_step(ST_PMIC_INIT, EV_PMIC_FAIL, &g);
    CHECK_EQ_INT(o.next, ST_ERROR);

    /* --- provisioning --- */
    /* A device with no stored SSID opens the portal instead of failing to
     * associate five times against a network it was never told about. */
    g = all_ready();
    g.provisioned = false;
    o = sm_step(ST_PMIC_INIT, EV_PMIC_OK, &g);
    CHECK_EQ_INT(o.next, ST_PROVISIONING);
    CHECK(o.actions & ACT_PROV_START);
    CHECK(!(o.actions & ACT_WIFI_START));

    o = sm_step(ST_PROVISIONING, EV_PROVISIONED, &g);
    CHECK(o.actions & ACT_REBOOT);

    /* The portal is reachable from every state a stuck user can be in. */
    g = all_ready();
    o = sm_step(ST_IDLE_READY, EV_ENTER_SETUP, &g);
    CHECK_EQ_INT(o.next, ST_PROVISIONING);
    CHECK(o.actions & ACT_PROV_START);
    o = sm_step(ST_NOT_READY, EV_ENTER_SETUP, &g);
    CHECK_EQ_INT(o.next, ST_PROVISIONING);
    o = sm_step(ST_ERROR, EV_ENTER_SETUP, &g);
    CHECK_EQ_INT(o.next, ST_PROVISIONING);
    o = sm_step(ST_TIME_SYNC, EV_ENTER_SETUP, &g);
    CHECK_EQ_INT(o.next, ST_PROVISIONING);
    /* Above all, from a wrong password: without this the user waits out the
     * retry ladder every single boot. */
    o = sm_step(ST_WIFI_CONNECTING, EV_ENTER_SETUP, &g);
    CHECK_EQ_INT(o.next, ST_PROVISIONING);
    CHECK(o.actions & ACT_PROV_START);

    /* Setup wins over retry when both could fire on the error screen. */
    g = all_ready();
    g.wifi_up = false;
    o = sm_step(ST_ERROR, EV_ENTER_SETUP, &g);
    CHECK_EQ_INT(o.next, ST_PROVISIONING);

    /* An unprovisioned device must not auto-clear its error screen into an
     * idle state it can never satisfy. */
    g = all_ready();
    g.provisioned = false;
    o = sm_step(ST_ERROR, EV_TIMEOUT, &g);
    CHECK_EQ_INT(o.next, ST_ERROR);
    CHECK_EQ_INT(o.actions, ACT_NONE);

    /* Recording must never begin while the portal is open. */
    g = all_ready();
    o = sm_step(ST_PROVISIONING, EV_BTN_PRESS, &g);
    CHECK_EQ_INT(o.next, ST_PROVISIONING);
    CHECK(!(o.actions & ACT_REC_START));

    g = all_ready();

    o = sm_step(ST_WIFI_CONNECTING, EV_WIFI_UP, &g);
    CHECK_EQ_INT(o.next, ST_TIME_SYNC);
    CHECK(o.actions & ACT_SNTP_START);

    /* A trusted local HTTP endpoint is useful without internet access. It
     * must not wait forever for a clock it does not need. */
    g.time_required = false;
    g.time_ok = false;
    o = sm_step(ST_WIFI_CONNECTING, EV_WIFI_UP, &g);
    CHECK_EQ_INT(o.next, ST_IDLE_READY);
    CHECK_EQ_INT(o.actions, ACT_NONE);
    o = sm_step(ST_IDLE_READY, EV_BTN_PRESS, &g);
    CHECK_EQ_INT(o.next, ST_RECORDING);
    g.time_required = true;
    g.time_ok = true;

    /* Wi-Fi retries back off, then give up. A failed association arrives as
     * EV_WIFI_DOWN, not EV_TIMEOUT, so both must drive the ladder — otherwise
     * the device sits in WIFI_CONNECTING forever with a wrong password. */
    for (int i = 0; i < 2; i++) {
        const app_event_t drop = i == 0 ? EV_TIMEOUT : EV_WIFI_DOWN;
        g.wifi_retries = 0;
        o = sm_step(ST_WIFI_CONNECTING, drop, &g);
        CHECK_EQ_INT(o.next, ST_WIFI_CONNECTING);
        CHECK(o.actions & ACT_WIFI_RETRY);
        g.wifi_retries = SM_WIFI_MAX_RETRIES;
        o = sm_step(ST_WIFI_CONNECTING, drop, &g);
        CHECK_EQ_INT(o.next, ST_ERROR);
    }
    g.wifi_retries = 0;

    /* An unsynced clock is fatal: TLS cannot validate a cert at epoch 1970,
     * so there is no point pretending we are ready. */
    g = all_ready();
    o = sm_step(ST_TIME_SYNC, EV_TIME_OK, &g);   CHECK_EQ_INT(o.next, ST_IDLE_READY);
    o = sm_step(ST_TIME_SYNC, EV_TIME_FAIL, &g); CHECK_EQ_INT(o.next, ST_ERROR);

    /* --- the happy path --- */
    g = all_ready();
    o = sm_step(ST_IDLE_READY, EV_BTN_PRESS, &g);
    CHECK_EQ_INT(o.next, ST_RECORDING);
    CHECK(o.actions & ACT_REC_START);

    o = sm_step(ST_RECORDING, EV_BTN_RELEASE, &g);
    CHECK_EQ_INT(o.next, ST_UPLOADING);
    CHECK(o.actions & ACT_REC_STOP);
    CHECK(o.actions & ACT_UPLOAD_START);

    o = sm_step(ST_UPLOADING, EV_STT_OK, &g);
    CHECK_EQ_INT(o.next, ST_TYPING);
    CHECK(o.actions & ACT_TYPE_START);

    o = sm_step(ST_TYPING, EV_TYPE_DONE, &g);
    CHECK_EQ_INT(o.next, ST_IDLE_READY);
    CHECK(o.actions & ACT_CLIP_DISCARD);

    /* --- press-lost commits, it does not discard --- */
    g = all_ready();
    o = sm_step(ST_RECORDING, EV_PRESS_LOST, &g);
    CHECK_EQ_INT(o.next, ST_UPLOADING);
    CHECK(o.actions & ACT_UPLOAD_START);
    CHECK(!(o.actions & ACT_CLIP_DISCARD));

    /* ...unless the clip is a tap or silence, in which case both release and
     * press-lost drop it and say nothing was heard. */
    g.clip_usable = false;
    o = sm_step(ST_RECORDING, EV_BTN_RELEASE, &g);
    CHECK_EQ_INT(o.next, ST_IDLE_READY);
    CHECK(o.actions & ACT_CLIP_DISCARD);
    CHECK(!(o.actions & ACT_UPLOAD_START));
    o = sm_step(ST_RECORDING, EV_PRESS_LOST, &g);
    CHECK_EQ_INT(o.next, ST_IDLE_READY);
    CHECK(o.actions & ACT_CLIP_DISCARD);

    /* --- max duration uploads immediately and swallows the late release --- */
    g = all_ready();
    o = sm_step(ST_RECORDING, EV_REC_MAX, &g);
    CHECK_EQ_INT(o.next, ST_UPLOADING);
    CHECK(o.actions & ACT_UPLOAD_START);
    /* The finger is still down. Its eventual release must not disturb the
     * in-flight upload. */
    o = sm_step(ST_UPLOADING, EV_BTN_RELEASE, &g);
    CHECK_EQ_INT(o.next, ST_UPLOADING);
    CHECK_EQ_INT(o.actions, ACT_NONE);
    o = sm_step(ST_UPLOADING, EV_PRESS_LOST, &g);
    CHECK_EQ_INT(o.next, ST_UPLOADING);
    CHECK_EQ_INT(o.actions, ACT_NONE);

    /* --- readiness gating --- */
    g = all_ready();
    g.usb_mounted = false;
    o = sm_step(ST_IDLE_READY, EV_BTN_PRESS, &g);
    CHECK_EQ_INT(o.next, ST_IDLE_READY);
    CHECK(o.actions & ACT_HINT_NOT_READY);
    CHECK(!(o.actions & ACT_REC_START));

    g = all_ready();
    g.time_ok = false;
    o = sm_step(ST_IDLE_READY, EV_BTN_PRESS, &g);
    CHECK(o.actions & ACT_HINT_NOT_READY);

    o = sm_step(ST_IDLE_READY, EV_USB_UNMOUNT, ready());
    CHECK_EQ_INT(o.next, ST_NOT_READY);
    o = sm_step(ST_IDLE_READY, EV_WIFI_DOWN, ready());
    CHECK_EQ_INT(o.next, ST_NOT_READY);

    /* Recovering one precondition is not enough; the guard checks the rest. */
    g = all_ready();
    g.wifi_up = false;
    o = sm_step(ST_NOT_READY, EV_USB_MOUNT, &g);
    CHECK_EQ_INT(o.next, ST_NOT_READY);
    g = all_ready();
    o = sm_step(ST_NOT_READY, EV_USB_MOUNT, &g);
    CHECK_EQ_INT(o.next, ST_IDLE_READY);

    /* --- losing things mid-flight --- */
    g = all_ready();
    o = sm_step(ST_RECORDING, EV_WIFI_DOWN, &g);
    CHECK_EQ_INT(o.next, ST_NOT_READY);
    CHECK(o.actions & ACT_REC_STOP);
    CHECK(o.actions & ACT_CLIP_DISCARD);

    o = sm_step(ST_UPLOADING, EV_WIFI_DOWN, &g);
    CHECK_EQ_INT(o.next, ST_ERROR);
    CHECK(o.actions & ACT_UPLOAD_ABORT);

    o = sm_step(ST_TYPING, EV_USB_UNMOUNT, &g);
    CHECK_EQ_INT(o.next, ST_ERROR);
    CHECK(o.actions & ACT_TYPE_ABORT);

    /* Silence returns to idle quietly rather than raising an error. */
    o = sm_step(ST_UPLOADING, EV_STT_EMPTY, &g);
    CHECK_EQ_INT(o.next, ST_IDLE_READY);
    CHECK(o.actions & ACT_CLIP_DISCARD);
    CHECK(!(o.actions & ACT_SHOW_ERROR));

    o = sm_step(ST_UPLOADING, EV_STT_FAIL, &g);
    CHECK_EQ_INT(o.next, ST_ERROR);

    /* --- error recovery --- */
    g = all_ready();
    o = sm_step(ST_ERROR, EV_RETRY, &g);
    CHECK_EQ_INT(o.next, ST_IDLE_READY);
    g.wifi_up = false;
    o = sm_step(ST_ERROR, EV_RETRY, &g);
    CHECK_EQ_INT(o.next, ST_WIFI_CONNECTING);
    o = sm_step(ST_ERROR, EV_TIMEOUT, ready());
    CHECK_EQ_INT(o.next, ST_IDLE_READY);

    /* --- Send and Undo act on a just-typed dictation --- */
    /* A finished transcript leaves something pending; Send fires the chord and
     * parks in ST_SENDING until the typing task reports back. */
    g = all_ready();
    o = sm_step(ST_IDLE_READY, EV_SEND, &g);
    CHECK_EQ_INT(o.next, ST_SENDING);
    CHECK(o.actions & ACT_SEND_KEY);
    CHECK(!(o.actions & ACT_UNDO));

    o = sm_step(ST_IDLE_READY, EV_UNDO, &g);
    CHECK_EQ_INT(o.next, ST_SENDING);
    CHECK(o.actions & ACT_UNDO);
    CHECK(!(o.actions & ACT_SEND_KEY));

    /* Nothing pending: the buttons are dimmed, and a stray tap is inert. */
    g.has_pending = false;
    o = sm_step(ST_IDLE_READY, EV_SEND, &g);
    CHECK_EQ_INT(o.next, ST_IDLE_READY);
    CHECK_EQ_INT(o.actions, ACT_NONE);
    o = sm_step(ST_IDLE_READY, EV_UNDO, &g);
    CHECK_EQ_INT(o.next, ST_IDLE_READY);
    CHECK_EQ_INT(o.actions, ACT_NONE);

    /* No host to type into: refuse rather than fire into nothing. */
    g = all_ready();
    g.usb_mounted = false;
    o = sm_step(ST_IDLE_READY, EV_SEND, &g);
    CHECK_EQ_INT(o.next, ST_IDLE_READY);
    CHECK_EQ_INT(o.actions, ACT_NONE);

    /* The chord (or the backspaces) finishing lands back on idle with no clip
     * to discard; a host yanked mid-send is an error, same as typing. */
    o = sm_step(ST_SENDING, EV_TYPE_DONE, ready());
    CHECK_EQ_INT(o.next, ST_IDLE_READY);
    CHECK_EQ_INT(o.actions, ACT_NONE);
    o = sm_step(ST_SENDING, EV_TYPE_ABORT, ready());
    CHECK_EQ_INT(o.next, ST_ERROR);
    o = sm_step(ST_SENDING, EV_USB_UNMOUNT, ready());
    CHECK_EQ_INT(o.next, ST_ERROR);
    CHECK(o.actions & ACT_TYPE_ABORT);

    /* --- total function: no (state, event) pair may crash or wander --- */
    g = all_ready();
    for (int s = 0; s < ST_COUNT; s++) {
        for (int e = 0; e < EV_COUNT; e++) {
            o = sm_step((app_state_t)s, (app_event_t)e, &g);
            CHECK(o.next >= 0 && o.next < ST_COUNT);
        }
    }
    /* Same sweep with every guard false, which is where a missing guard check
     * would let us record without Wi-Fi. */
    sm_guards_t none = {0};
    for (int s = 0; s < ST_COUNT; s++) {
        for (int e = 0; e < EV_COUNT; e++) {
            o = sm_step((app_state_t)s, (app_event_t)e, &none);
            CHECK(o.next >= 0 && o.next < ST_COUNT);
            if ((app_state_t)s == ST_IDLE_READY) {
                CHECK(!(o.actions & ACT_REC_START));
            }
            /* The portal must never start a recording or an upload, in any
             * state it can be entered from, under any guard combination. */
            if ((app_state_t)s == ST_PROVISIONING) {
                CHECK(!(o.actions & (ACT_REC_START | ACT_UPLOAD_START | ACT_TYPE_START)));
            }
        }
    }

    /* Unhandled pairs are inert, not silent state changes. */
    o = sm_step(ST_IDLE_READY, EV_TYPE_DONE, ready());
    CHECK_EQ_INT(o.next, ST_IDLE_READY);
    CHECK_EQ_INT(o.actions, ACT_NONE);

    CHECK_EQ_STR(sm_state_name(ST_RECORDING), "RECORDING");

    /* Every event and state must have a name. A log line reading "--10-->"
     * costs a trip to the header; "--REC_MAX-->" says a duration cap fired
     * rather than a finger lifting. */
    for (int e = 0; e < EV_COUNT; e++) {
        const char *n = sm_event_name((app_event_t)e);
        CHECK(n != NULL);
        CHECK(n[0] != '\0');
        CHECK_EQ_STR(n, n[0] == '?' ? "unnamed event" : n); /* '?' means forgotten */
    }
    for (int s = 0; s < ST_COUNT; s++) {
        const char *n = sm_state_name((app_state_t)s);
        CHECK(n != NULL);
        CHECK(n[0] != '?');
    }
})
