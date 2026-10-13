// SPDX-License-Identifier: GPL-2.0-or-later

#include "hle/net/net.h"

#include <arpa/inet.h>
#include <cstring>

#include "common/log.h"
#include "hle/hle.h"

namespace Hle::Net {

namespace {

struct OrbisNetEtherAddr {
    u8 data[6];
};

struct OrbisNetCtlInfo {
    u32 device;
    OrbisNetEtherAddr ether_addr;
    u32 mtu;
    u32 link;
    char ip_address[16];
    char netmask[16];
    char default_route[16];
    char primary_dns[16];
    char secondary_dns[16];
    u32 http_proxy_config;
    char http_proxy_server[256];
    u16 http_proxy_port;
    u8 reserved[2];
};

s32 sceNetGetMacAddress(OrbisNetEtherAddr* addr, s32 flags) {
    if (addr) {
        addr->data[0] = 0x00;
        addr->data[1] = 0xd9;
        addr->data[2] = 0xd1;
        addr->data[3] = 0x12;
        addr->data[4] = 0x34;
        addr->data[5] = 0x56;
    }
    return 0;
}

s32 sceNetPoolCreate(const char* name, s32 size, s32 flags) {
    DC_INFO("sceNetPoolCreate(name=%s, size=%d, flags=%#x) -> pool 1", name ? name : "", size, flags);
    return 1;
}

s32 sceNetCtlInit() {
    return 0;
}

s32 sceNetCtlTerm() {
    return 0;
}

s32 sceNetCtlGetInfo(s32 code, OrbisNetCtlInfo* info) {
    if (!info) {
        return -1;
    }
    info->device = 1; // wired
    info->ether_addr.data[0] = 0x00;
    info->ether_addr.data[1] = 0xd9;
    info->ether_addr.data[2] = 0xd1;
    info->ether_addr.data[3] = 0x12;
    info->ether_addr.data[4] = 0x34;
    info->ether_addr.data[5] = 0x56;
    info->mtu = 1500;
    info->link = 1; // connected
    std::strncpy(info->ip_address, "192.168.1.100", sizeof(info->ip_address));
    std::strncpy(info->netmask, "255.255.255.0", sizeof(info->netmask));
    std::strncpy(info->default_route, "192.168.1.1", sizeof(info->default_route));
    std::strncpy(info->primary_dns, "1.1.1.1", sizeof(info->primary_dns));
    return 0;
}

s32 sceNetInit() {
    return 0;
}

s32 sceNetCtlGetState(s32* state) {
    if (state) {
        *state = 3; // ORBIS_NET_CTL_STATE_IPOBTAINED
    }
    return 0;
}

s32 sceNetCtlRegisterCallback(void* cb, void* arg, s32* cid) {
    if (cid) {
        *cid = 1;
    }
    return 0;
}

s32 sceNetCtlCheckCallback() {
    return 0;
}

u16 sceNetHtons(u16 n) {
    return __builtin_bswap16(n);
}

u16 sceNetNtohs(u16 n) {
    return __builtin_bswap16(n);
}

u32 sceNetHtonl(u32 n) {
    return __builtin_bswap32(n);
}

u32 sceNetNtohl(u32 n) {
    return __builtin_bswap32(n);
}

s32 sceNetResolverCreate(const char* name, void* pool, s32 flags) {
    return 1;
}

s32 sceNetResolverDestroy(s32 id) {
    return 0;
}

s32 sceNetInetPton(s32 af, const char* src, void* dst) {
    if (!src || !dst) {
        return -1;
    }
    const int host_af = (af == 2) ? AF_INET : (af == 28 ? AF_INET6 : AF_INET);
    return inet_pton(host_af, src, dst);
}

s32 sceNetSocket(const char* name, s32 family, s32 type, s32 protocol) {
    (void)name;
    (void)family;
    (void)type;
    (void)protocol;
    static s32 next_sock = 100;
    return next_sock++;
}

s32 sceNetSetsockopt(s32 s, s32 level, s32 optname, const void* optval, s32 optlen) {
    (void)s;
    (void)level;
    (void)optname;
    (void)optval;
    (void)optlen;
    return 0;
}

s32 sceNetGetsockopt(s32 s, s32 level, s32 optname, void* optval, s32* optlen) {
    (void)s;
    (void)level;
    (void)optname;
    (void)optval;
    (void)optlen;
    return 0;
}

s32 sceNetClose(s32 s) {
    (void)s;
    return 0;
}

} // namespace

void RegisterNet() {
    DC_HLE("libSceNet", sceNetInit);
    DC_HLE("libSceNet", sceNetGetMacAddress);
    DC_HLE("libSceNet", sceNetPoolCreate);
    DC_HLE("libSceNet", sceNetHtons);
    DC_HLE("libSceNet", sceNetNtohs);
    DC_HLE("libSceNet", sceNetHtonl);
    DC_HLE("libSceNet", sceNetNtohl);
    DC_HLE("libSceNet", sceNetResolverCreate);
    DC_HLE("libSceNet", sceNetResolverDestroy);
    DC_HLE("libSceNet", sceNetInetPton);
    DC_HLE("libSceNet", sceNetSocket);
    DC_HLE("libSceNet", sceNetSetsockopt);
    DC_HLE("libSceNet", sceNetGetsockopt);
    DC_HLE("libSceNet", sceNetClose);
    DC_HLE("libSceNetCtl", sceNetCtlInit);
    DC_HLE("libSceNetCtl", sceNetCtlTerm);
    DC_HLE("libSceNetCtl", sceNetCtlGetInfo);
    DC_HLE("libSceNetCtl", sceNetCtlGetState);
    DC_HLE("libSceNetCtl", sceNetCtlRegisterCallback);
    DC_HLE("libSceNetCtl", sceNetCtlCheckCallback);
}

} // namespace Hle::Net
