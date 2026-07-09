/* Captive-portal DNS: answer every A query with our own address.
 *
 * This parses packets from anything that associates with the setup access
 * point, so it is pure and heavily tested rather than trusted. Malformed input
 * is dropped, never partially answered. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define DNS_MIN_PACKET 12 /* header only */

/* Rewrite `pkt` (a DNS query of `len` bytes, in a buffer of `cap` bytes) into
 * a reply that resolves the queried name to `ip`.
 *
 * Returns the reply length, or 0 if the packet should be dropped: too short,
 * not a query, no question, malformed labels, or no room for the answer. */
size_t dns_build_reply(uint8_t *pkt, size_t len, size_t cap, const uint8_t ip[4]);
