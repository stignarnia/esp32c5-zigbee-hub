// lwIP hooks of the hub, compiled into the lwip component through
// ESP_IDF_LWIP_HOOK_FILENAME (set by components/tailscale/CMakeLists.txt).
// Included from lwipopts.h: no lwIP includes here, forward declarations only.
#pragma once

struct pbuf;
struct netif;

// Drops IPv4 packets that arrive on the Wi-Fi interface addressed to our tailnet IP.
// lwIP is a weak host: it accepts a packet for any of its addresses on any
// interface, so a LAN host could otherwise reach the tailnet-only services by
// sending to 100.x directly. Tailnet traffic arrives decrypted on the WireGuard
// interface, never on Wi-Fi. Returns 1 (and frees the pbuf) when dropped.
int hub_ip4_input_hook(struct pbuf *p, struct netif *inp);
#define LWIP_HOOK_IP4_INPUT(p, inp) hub_ip4_input_hook((p), (inp))
