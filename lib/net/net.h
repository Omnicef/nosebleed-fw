// SPDX-License-Identifier: GPL-3.0-only
//
// T-9.2 / T-9.1 — WiFi supervision on the net task. STA connect from
// NVS-stored credentials (compile-time secrets.h as build fallback,
// saved credentials win), exponential backoff on failure, SoftAP
// provisioning fallback after sustained failure (open AP + catch-all
// DNS; the portal page and POST /api/net/connect live in lib/web/),
// and unattended recovery: an idle AP is periodically reprobed against
// the saved network so a router reboot needs nobody.
//
// Credentials are never logged and never surface on any endpoint; the
// logs name the SOURCE of the credentials, never the values.

#pragma once

#ifdef ARDUINO

namespace nb {
namespace net {

// Compute the AP SSID and load the active credential set (NVS first,
// build-time secrets.h fallback). Call once after WiFi is powered up.
void init();

// Advance the supervision machine. Call every ~500 ms from one task.
void tick();

// STA connected, or the provisioning AP is up — the web server gate.
bool link_ready();

bool ap_active();
const char* ap_ssid();  // valid after init(), even before the AP is up
const char* ap_ip();    // "192.168.4.1" while the AP is up, else ""

// One-shot edges for the caller (panel splash, SNTP arming): each
// returns true exactly once per transition.
bool take_edge_up();     // newly associated with the saved network
bool take_edge_ap_up();  // provisioning AP just came up

// Called by POST /api/net/connect after save_creds(): drop the backoff
// and re-attempt with the fresh credentials on the next tick.
void request_reconnect();

}  // namespace net
}  // namespace nb

#endif  // ARDUINO
