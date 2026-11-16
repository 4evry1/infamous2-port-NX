/* Horizon layer: <ifaddrs.h>. The port is offline: getifaddrs reports no interfaces. */
#ifndef HZ_IFADDRS_H
#define HZ_IFADDRS_H
#include <sys/socket.h>
#ifdef __cplusplus
extern "C" {
#endif
struct ifaddrs {
    struct ifaddrs *ifa_next;
    char *ifa_name;
    unsigned int ifa_flags;
    struct sockaddr *ifa_addr;
    struct sockaddr *ifa_netmask;
    struct sockaddr *ifa_broadaddr;
    void *ifa_data;
};
int getifaddrs(struct ifaddrs **ifap);
void freeifaddrs(struct ifaddrs *ifa);
#ifdef __cplusplus
}
#endif
#endif
