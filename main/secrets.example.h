/* OPTIONAL development convenience.
 *
 * The firmware does not need this file. With no stored credentials it raises a
 * setup access point and asks for them; that is the normal path.
 *
 * If you copy this to main/secrets.h and fill it in, config_store.c seeds NVS
 * from it on first boot so a bench build skips the portal. secrets.h is
 * gitignored. Never build a binary you intend to share from a tree that
 * contains it -- your credentials end up inside the image.
 *
 * See docs/security.md. */
#pragma once

#define TAPTALK_WIFI_SSID     "your-ssid"
#define TAPTALK_WIFI_PASSWORD "your-password"

/* Unused until the STT client lands in chunk 2. */
#define TAPTALK_OPENAI_API_KEY "sk-..."
