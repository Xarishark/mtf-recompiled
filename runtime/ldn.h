// Clean-room Horizon OS nn::ldn (Link Play) & nn::socket UDP HLE over Winsock2.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- nn::ldn ABI constants & structs (Horizon OS)
enum {
    LDN_STATE_NONE = 0,
    LDN_STATE_INITIALIZED = 1,
    LDN_STATE_ACCESS_POINT_OPENED = 2,
    LDN_STATE_ACCESS_POINT_CREATED = 3,
    LDN_STATE_STATION_OPENED = 4,
    LDN_STATE_STATION_CONNECTED = 5,
    LDN_STATE_ERROR = 6,
};

enum {
    LDN_RES_OK = 0,
    LDN_RES_UNAVAILABLE = 0x2A2u,
    LDN_RES_INVALID_STATE = 0x20A2u,
    LDN_RES_CONNECTION_FAILED = 0x2CA2u,
};

#pragma pack(push, 1)
typedef struct {
    uint8_t raw[16]; // IntentId (16 bytes)
    uint8_t session_id[16];
} LdnNetworkId;

typedef struct {
    uint8_t bssid[6];
    uint8_t ssid_len;
    char ssid[33];
    int16_t channel;
    int8_t link_level;
    uint8_t network_type;
    uint8_t reserved[4];
} LdnCommonNetworkInfo;

typedef struct {
    uint32_t ipv4_addr;   // host byte order (e.g. 0x0A720001 = 10.114.0.1)
    uint8_t mac[6];
    int8_t node_id;
    uint8_t is_connected;
    char user_name[33];
    uint8_t reserved1[1];
    int16_t local_comm_version;
    uint8_t reserved2[16];
} LdnNodeInfo;

typedef struct {
    uint8_t node_count_max;
    uint8_t node_count;
    uint8_t reserved1[2];
    uint8_t security_param[16];
    uint16_t security_mode;
    uint8_t accept_policy;
    uint8_t reserved2[3];
    uint8_t node_latest_update[8];
    LdnNodeInfo nodes[8];
    uint16_t advertise_data_size;
    uint8_t advertise_data[384];
    uint8_t reserved3[140];
} LdnSpecificNetworkInfo;

typedef struct {
    LdnNetworkId network_id;
    LdnCommonNetworkInfo common;
    LdnSpecificNetworkInfo ldn;
} LdnNetworkInfo; // 0x480 bytes (1152 bytes)

typedef struct {
    uint8_t intent_id[16];
    int16_t channel;
    uint8_t node_count_max;
    uint8_t reserved1;
    uint16_t local_comm_version;
    uint8_t reserved2[10];
} LdnNetworkConfig;

typedef struct {
    uint16_t security_mode;
    uint16_t passphrase_size;
    uint8_t passphrase[64];
} LdnSecurityConfig;

typedef struct {
    char user_name[33];
    uint8_t reserved[15];
} LdnUserConfig;

typedef struct {
    LdnNetworkId network_id;
    uint32_t network_type;
    uint8_t bssid[6];
    uint8_t ssid_len;
    char ssid[33];
    uint8_t reserved[16];
    uint32_t flag;
} LdnScanFilter;
#pragma pack(pop)

_Static_assert(sizeof(LdnNodeInfo) == 0x40, "LdnNodeInfo size");
_Static_assert(sizeof(LdnNetworkInfo) == 0x480, "LdnNetworkInfo size");

int ldn_enabled(void);

// nn::ldn lifecycle & state
uint32_t ldn_initialize(void);
void ldn_finalize(void);
uint32_t ldn_get_state(void);
void ldn_attach_state_event(uint64_t event_addr);

uint32_t ldn_open_access_point(void);
uint32_t ldn_close_access_point(void);
uint32_t ldn_open_station(void);
uint32_t ldn_close_station(void);

uint32_t ldn_create_network(const LdnNetworkConfig *nc, const LdnSecurityConfig *sc, const LdnUserConfig *uc);
uint32_t ldn_destroy_network(void);
uint32_t ldn_set_advertise_data(const void *data, uint64_t size);

uint32_t ldn_scan(LdnNetworkInfo *out, int32_t *count, int32_t max_count, const LdnScanFilter *filter);
uint32_t ldn_connect(const LdnNetworkInfo *info, const LdnSecurityConfig *sc, const LdnUserConfig *uc);
uint32_t ldn_disconnect(void);

uint32_t ldn_get_network_info(LdnNetworkInfo *out);
uint32_t ldn_get_ipv4_address(uint32_t *addr_out, uint32_t *mask_out);
uint32_t ldn_get_security_parameter(uint8_t out[32]);

// nn::nifm helpers when LDN is active
int ldn_nifm_available(void);
uint32_t ldn_nifm_ip_info(uint32_t *ip, uint32_t *mask, uint32_t *gw, uint32_t *dns1, uint32_t *dns2);

// nn::socket UDP backend
int ldn_sock_socket(int family, int type, int proto);
int ldn_sock_bind(int fd, const void *addr, uint32_t addrlen);
int ldn_sock_setsockopt(int fd, int level, int optname, const void *optval, uint32_t optlen);
int64_t ldn_sock_sendto(int fd, const void *buf, uint64_t len, int flags, const void *addr, uint32_t addrlen);
int64_t ldn_sock_recvfrom(int fd, void *buf, uint64_t len, int flags, void *addr, uint32_t *addrlen);
int ldn_sock_close(int fd);
int ldn_sock_shutdown(int fd, int how);
int ldn_sock_is_fd(int fd);
int ldn_sock_poll_ready(int fd, int16_t events, int16_t *revents);
int ldn_sock_last_errno(void);

#ifdef __cplusplus
}
#endif
