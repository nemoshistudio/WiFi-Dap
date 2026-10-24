#ifndef WT_MDNS_CONFIG_H_GUARD
#define WT_MDNS_CONFIG_H_GUARD

#define MDSN_HOSTNAME_PREFIX "wifi-dap" /* + "-" + last 4 MAC hex; .local via mDNS */
#define MDSN_INSTANCE_DESC "WiFi Dap"

void wt_mdns_init();

int wt_mdns_set_hostname(const char* hostname);


#endif //WT_MDNS_CONFIG_H_GUARD