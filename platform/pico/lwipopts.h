#pragma once

// lwip_freertos arch — lwIP runs in its own tcpip_thread, BSD sockets enabled
#define NO_SYS                          0
#define LWIP_SOCKET                     1
#define LWIP_NETCONN                    1

// tcpip thread (configMAX_PRIORITIES=8, so 6 is below the timer task at 7)
// TCP connection setup (SYN→PCB alloc→SYN-ACK) has a deeper call stack than
// UDP/mDNS; 1024 words was marginal and caused stack-overflow panics on connect.
#define TCPIP_THREAD_STACKSIZE          2048
#define TCPIP_THREAD_PRIO               6
#define TCPIP_MBOX_SIZE                 16
#define DEFAULT_TCP_RECVMBOX_SIZE       12
#define DEFAULT_UDP_RECVMBOX_SIZE       12
#define DEFAULT_ACCEPTMBOX_SIZE         8

// Thread safety
#define SYS_LIGHTWEIGHT_PROT            1
#define LWIP_TCPIP_CORE_LOCKING         1

// Sockets / netconns (default 4 is too few: mDNS + telnet server + telnet client)
#define MEMP_NUM_NETCONN                8

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

// Socket options
#define LWIP_SO_RCVTIMEO                1

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

// Avoid newlib conflicts
#define LWIP_POSIX_SOCKETS_IO_NAMES     0
#define LWIP_TIMEVAL_PRIVATE            0

// Stats off
#define LWIP_STATS                      0
#define LWIP_STATS_DISPLAY              0
