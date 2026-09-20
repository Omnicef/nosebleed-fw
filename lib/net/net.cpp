// SPDX-License-Identifier: GPL-3.0-only
//
// T-9.2 / T-9.1 — see net.h. APIs verified against installed headers:
// WiFiAP.h (softAP/softAPdisconnect/softAPIP/softAPgetStationNum),
// WiFiSTA.h (begin/status/disconnect), WiFiGeneric.h (macAddress),
// DNSServer.h (default ctor = captive catch-all; start() defaults to
// port 53 + the AP's IP), config/store.h (load_creds/save_creds).

#include "net.h"

#ifdef ARDUINO

#include <WiFi.h>
#include <DNSServer.h>

#include <cstdio>
#include <cstring>

#include "config.h"
#include "store.h"

// Build-time override (gitignored secrets.h, same contract as before
// T-9.2): development keeps working without re-provisioning every flash.
// Saved NVS credentials win when present. Included HERE, not just in
// main.cpp — first hardware run proved the macro is per-TU: with it only
// in main.cpp the state machine saw an empty SSID and provisioned.
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#define WIFI_PASS ""
#endif

namespace nb {
namespace net {
namespace {

constexpr uint32_t kAssocWindowMs = 10000;    // one association attempt's grace
constexpr uint32_t kBackoffCapMs = 30000;     // ceiling, same as the old fixed retry
constexpr int kFailuresToProvision = 3;       // 1+2+4 s of failure -> ask for a network
constexpr uint32_t kApProbeMs = 300000;      // unattended reprobe cadence while AP idles —
                                       // 30 s made the AP scan-storm and become
                                       // unjoinable (hardware-proven 2026-09-19)

DNSServer s_dns;
char s_ap_ssid[20] = "nosebleed";  // "nosebleed-" + 6 hex of MAC (16 chars)
char s_ap_ip[16] = "";
config::Creds s_creds = {};
bool s_ap = false;
bool s_assoc = false;
bool s_linked = false;
bool s_edge_up = false;
bool s_edge_ap = false;
bool s_dirty = false;  // fresh credentials saved; re-attempt now
bool s_force = false;   // one-shot: the dirty submission overrides the AP station gate
int s_attempts = 0;
uint32_t s_assoc_t = 0, s_retry_at = 0, s_probe_t = 0, s_diag_t = 0;

// Never prints ssid/pass — only the source. The values must not reach
// the serial line, ever.
void load_active() {
    std::memset(&s_creds, 0, sizeof(s_creds));
    if (config::load_creds(s_creds)) {
        Serial.println("[net] wifi target: saved credentials");
    } else {
        strncpy(s_creds.ssid, WIFI_SSID, sizeof(s_creds.ssid) - 1);
        strncpy(s_creds.pass, WIFI_PASS, sizeof(s_creds.pass) - 1);
        Serial.printf("[net] wifi target: %s\n",
                      s_creds.ssid[0] != '\0' ? "built-in (secrets.h)" : "none (provisioning)");
    }
}

void ap_up() {
    WiFi.mode(WIFI_AP_STA);  // STA stays powered for the periodic reprobe
    if (!WiFi.softAP(s_ap_ssid)) {  // open AP — it is a provisioning portal, not a network
        Serial.println("[net] softAP FAILED");
        return;
    }
    snprintf(s_ap_ip, sizeof(s_ap_ip), "%s", WiFi.softAPIP().toString().c_str());
    if (!s_dns.start())  // defaults: port 53, all domains -> AP IP (captive)
        Serial.println("[net] captive DNS failed — portal reachable by IP only");
    s_ap = true;
    s_probe_t = millis();
    s_edge_ap = true;
}

void ap_down() {
    s_dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    s_ap = false;
    s_ap_ip[0] = '\0';
}

}  // namespace

void init() {
    const String mac = WiFi.macAddress();  // "AA:BB:CC:DD:EE:FF" — STA base, exists pre-connect
    const char* m = mac.c_str();
    if (mac.length() == 17)
        snprintf(s_ap_ssid, sizeof(s_ap_ssid), "nosebleed-%c%c%c%c%c%c",
                 m[9], m[10], m[12], m[13], m[15], m[16]);  // last 3 bytes
    load_active();
}

void tick() {
    const uint32_t now = millis();

    // Fresh credentials ALWAYS win — checked before the LINKED early-return
    // (first hardware run proved that return swallowing the dirty flag on a
    // connected board: the POST saved but nothing re-attempted until the
    // next link loss). On a live link, drop it deliberately and retry from
    // zero so /api/net/connect works whether or not the old network works.
    if (s_dirty) {
        s_dirty = false;
        load_active();
        s_attempts = 0;
        s_assoc = false;
        s_retry_at = now;
        s_force = true;  // user submissions bypass the AP station gate below
        if (s_linked) {
            Serial.println("[net] creds changed, restarting link");
            WiFi.disconnect(false);
            s_linked = false;
        }
        return;
    }

    if (WiFi.status() == WL_CONNECTED) {
        if (!s_linked) {
            s_linked = true;
            s_assoc = false;
            s_attempts = 0;
            s_edge_up = true;
            if (s_ap) ap_down();  // single radio: the network wins over the portal
        }
#ifdef NB_D2_TRACE
        if (now - s_diag_t >= 30000) {
            s_diag_t = now;
            Serial.printf("[net] rssi=%d gw=%s dns=%s bssid=%s status=%d\n", WiFi.RSSI(),
                          WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP(0).toString().c_str(),
                          WiFi.BSSIDstr().c_str(), (int)WiFi.status());
        }
#endif
        return;
    }

    if (s_linked) {  // dropped — router reboot or walk-away. Retry from zero backoff.
        s_linked = false;
        s_attempts = 0;
        s_retry_at = now;
        Serial.println("[net] link lost");
    }

    if (s_assoc) {
        if (now - s_assoc_t < kAssocWindowMs) return;
        s_assoc = false;
        ++s_attempts;
        const uint32_t backoff = s_attempts >= 6 ? kBackoffCapMs : (1000u << (s_attempts - 1));
        s_retry_at = now + backoff;
        Serial.printf("[net] wifi attempt %d failed, retry in %lus\n", s_attempts,
                      static_cast<unsigned long>(backoff / 1000));
        if (!s_ap && s_attempts >= kFailuresToProvision) ap_up();
        return;
    }

    if (static_cast<int32_t>(now - s_retry_at) < 0) return;

    if (s_creds.ssid[0] == '\0') {
        if (!s_ap) ap_up();  // first boot, nothing stored: provision now
        return;
    }

    if (s_ap && !s_force) {
        // Unattended recovery (router came back): reprobe only while no
        // phone is hanging off the portal — associating moves the channel
        // and would kick whoever is still provisioning.
        if (WiFi.softAPgetStationNum() != 0) return;
        if (now - s_probe_t < kApProbeMs) return;
        s_probe_t = now;
        Serial.println("[net] AP idle, reprobing saved network");
    } else if (s_ap) {
        s_force = false;
        Serial.println("[net] provisioning handover, trying saved network");
    }

    WiFi.begin(s_creds.ssid, s_creds.pass);
    s_assoc = true;
    s_assoc_t = now;
}

bool link_ready() { return s_linked || s_ap; }
bool ap_active() { return s_ap; }
const char* ap_ssid() { return s_ap_ssid; }
const char* ap_ip() { return s_ap_ip; }

bool take_edge_up() {
    const bool e = s_edge_up;
    s_edge_up = false;
    return e;
}

bool take_edge_ap_up() {
    const bool e = s_edge_ap;
    s_edge_ap = false;
    return e;
}

void request_reconnect() { s_dirty = true; }

}  // namespace net
}  // namespace nb

#endif  // ARDUINO
