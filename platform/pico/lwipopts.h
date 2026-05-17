#pragma once

// threadsafe_background arch — lwIP runs from CYW43 background context, no tcpip thread
#define NO_SYS                          1
#define LWIP_SOCKET                     0
#define LWIP_NETCONN                    0

// Memory
#define MEM_LIBC_MALLOC                 0
#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        (16 * 1024)
#define MEMP_NUM_SYS_TIMEOUT            20
#define MEMP_NUM_TCP_SEG                32
#define PBUF_POOL_SIZE                  24

// Protocols
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_ICMP                       1
#define LWIP_IPV4                       1
#define LWIP_TCP                        1
#define LWIP_UDP                        1
#define LWIP_DHCP                       1
#define LWIP_DNS                        1
#define LWIP_RAW                        0

// TCP tuning
#define TCP_MSS                         1460
#define TCP_WND                         (8 * TCP_MSS)
#define TCP_SND_BUF                     (8 * TCP_MSS)
#define TCP_SND_QUEUELEN                ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))

// Netif
#define LWIP_NETIF_STATUS_CALLBACK      1
#define LWIP_NETIF_LINK_CALLBACK        1
#define LWIP_NETIF_HOSTNAME             1

// mDNS — requires IGMP for multicast
#define LWIP_IGMP                       1
#define LWIP_MDNS_RESPONDER             1
#define MDNS_MAX_SERVICES               4
#define LWIP_NUM_NETIF_CLIENT_DATA      (LWIP_MDNS_RESPONDER)

// No OS threading in lwIP — cyw43_arch_lwip_begin/end provides mutual exclusion
#define SYS_LIGHTWEIGHT_PROT            0
#define LWIP_TCPIP_CORE_LOCKING         0

// Avoid newlib conflicts
#define LWIP_POSIX_SOCKETS_IO_NAMES     0
#define LWIP_TIMEVAL_PRIVATE            0

// Stats off
#define LWIP_STATS                      0
#define LWIP_STATS_DISPLAY              0
