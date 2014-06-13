/*
 * Copyright 2012 Daniel Drown
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * clatd.c - tun interface setup and main event loop
 */
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>

#include <sys/capability.h>
#include <sys/uio.h>
#include <linux/prctl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <linux/if_ether.h>

#include <private/android_filesystem_config.h>

#include "translate.h"
#include "clatd.h"
#include "config.h"
#include "logging.h"
#include "resolv_netid.h"
#include "setif.h"
#include "setroute.h"
#include "mtu.h"
#include "getaddr.h"
#include "dump.h"

#define DEVICENAME6 "clat"
#define DEVICENAME4 "clat4"

/* 40 bytes IPv6 header - 20 bytes IPv4 header + 8 bytes fragment header */
#define MTU_DELTA 28

int forwarding_fd = -1;
volatile sig_atomic_t running = 1;

/* function: set_forwarding
 * enables/disables ipv6 forwarding
 */
void set_forwarding(int fd, const char *setting) {
  /* we have to forward packets from the WAN to the tun interface */
  if(write(fd, setting, strlen(setting)) < 0) {
    logmsg(ANDROID_LOG_FATAL,"set_forwarding(%s) failed: %s", setting, strerror(errno));
    exit(1);
  }
}

/* function: stop_loop
 * signal handler: stop the event loop
 */
void stop_loop() {
  running = 0;
}

/* function: tun_open
 * tries to open the tunnel device
 */
int tun_open() {
  int fd;

  fd = open("/dev/tun", O_RDWR);
  if(fd < 0) {
    fd = open("/dev/net/tun", O_RDWR);
  }

  return fd;
}

/* function: tun_alloc
 * creates a tun interface and names it
 * dev - the name for the new tun device
 */
int tun_alloc(char *dev, int fd) {
  struct ifreq ifr;
  int err;

  memset(&ifr, 0, sizeof(ifr));

  ifr.ifr_flags = IFF_TUN;
  if( *dev ) {
    strncpy(ifr.ifr_name, dev, IFNAMSIZ);
    ifr.ifr_name[IFNAMSIZ-1] = '\0';
  }

  if( (err = ioctl(fd, TUNSETIFF, (void *) &ifr)) < 0 ){
    close(fd);
    return err;
  }
  strcpy(dev, ifr.ifr_name);
  return 0;
}

/* function: deconfigure_tun_ipv6
 * removes the ipv6 route
 * tunnel - tun device data
 */
void deconfigure_tun_ipv6(const struct tun_data *tunnel) {
  int status;

  status = if_route(tunnel->device6, AF_INET6, &Global_Clatd_Config.ipv6_local_subnet,
      128, NULL, 1, 0, ROUTE_DELETE);
  if(status < 0) {
    logmsg(ANDROID_LOG_WARN,"deconfigure_tun_ipv6/if_route(6) failed: %s",strerror(-status));
  }
}

/* function: configure_tun_ipv6
 * configures the ipv6 route
 * note: routes a /128 out of the (assumed routed to us) /64 to the CLAT interface
 * tunnel - tun device data
 */
void configure_tun_ipv6(const struct tun_data *tunnel) {
  int status;

  status = if_route(tunnel->device6, AF_INET6, &Global_Clatd_Config.ipv6_local_subnet,
      128, NULL, 1, 0, ROUTE_CREATE);
  if(status < 0) {
    logmsg(ANDROID_LOG_FATAL,"configure_tun_ipv6/if_route(6) failed: %s",strerror(-status));
    exit(1);
  }
}

/* function: interface_poll
 * polls the uplink network interface for address changes
 * tunnel - tun device data
 */
void interface_poll(const struct tun_data *tunnel) {
  union anyip *interface_ip;

  interface_ip = getinterface_ip(Global_Clatd_Config.default_pdp_interface, AF_INET6);
  if(!interface_ip) {
    logmsg(ANDROID_LOG_WARN,"unable to find an ipv6 ip on interface %s",Global_Clatd_Config.default_pdp_interface);
    return;
  }

  config_generate_local_ipv6_subnet(&interface_ip->ip6);

  if(!IN6_ARE_ADDR_EQUAL(&interface_ip->ip6, &Global_Clatd_Config.ipv6_local_subnet)) {
    char from_addr[INET6_ADDRSTRLEN], to_addr[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &Global_Clatd_Config.ipv6_local_subnet, from_addr, sizeof(from_addr));
    inet_ntop(AF_INET6, &interface_ip->ip6, to_addr, sizeof(to_addr));
    logmsg(ANDROID_LOG_WARN, "clat subnet changed from %s to %s", from_addr, to_addr);

    // remove old route
    deconfigure_tun_ipv6(tunnel);

    // add new route, start translating packets to the new prefix
    memcpy(&Global_Clatd_Config.ipv6_local_subnet, &interface_ip->ip6, sizeof(struct in6_addr));
    configure_tun_ipv6(tunnel);
  }

  free(interface_ip);
}

/* function: configure_tun_ip
 * configures the ipv4 and ipv6 addresses on the tunnel interface
 * tunnel - tun device data
 */
void configure_tun_ip(const struct tun_data *tunnel) {
  int status;

  // Configure the interface before bringing it up. As soon as we bring the interface up, the
  // framework will be notified and will assume the interface's configuration has been finalized.
  status = add_address(tunnel->device4, AF_INET, &Global_Clatd_Config.ipv4_local_subnet,
      32, &Global_Clatd_Config.ipv4_local_subnet);
  if(status < 0) {
    logmsg(ANDROID_LOG_FATAL,"configure_tun_ip/if_address(4) failed: %s",strerror(-status));
    exit(1);
  }

  status = add_address(tunnel->device6, AF_INET6, &Global_Clatd_Config.ipv6_local_address,
      64, NULL);
  if(status < 0) {
    logmsg(ANDROID_LOG_FATAL,"configure_tun_ip/if_address(6) failed: %s",strerror(-status));
    exit(1);
  }

  if((status = if_up(tunnel->device6, Global_Clatd_Config.mtu)) < 0) {
    logmsg(ANDROID_LOG_FATAL,"configure_tun_ip/if_up(6) failed: %s",strerror(-status));
    exit(1);
  }

  if((status = if_up(tunnel->device4, Global_Clatd_Config.ipv4mtu)) < 0) {
    logmsg(ANDROID_LOG_FATAL,"configure_tun_ip/if_up(4) failed: %s",strerror(-status));
    exit(1);
  }

  configure_tun_ipv6(tunnel);
}

/* function: drop_root
 * drops root privs but keeps the needed capability
 */
void drop_root() {
  gid_t groups[] = { AID_INET };
  if(setgroups(sizeof(groups)/sizeof(groups[0]), groups) < 0) {
    logmsg(ANDROID_LOG_FATAL,"drop_root/setgroups failed: %s",strerror(errno));
    exit(1);
  }

  prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0);

  if(setgid(AID_CLAT) < 0) {
    logmsg(ANDROID_LOG_FATAL,"drop_root/setgid failed: %s",strerror(errno));
    exit(1);
  }
  if(setuid(AID_CLAT) < 0) {
    logmsg(ANDROID_LOG_FATAL,"drop_root/setuid failed: %s",strerror(errno));
    exit(1);
  }

  struct __user_cap_header_struct header;
  struct __user_cap_data_struct cap;
  memset(&header, 0, sizeof(header));
  memset(&cap, 0, sizeof(cap));

  header.version = _LINUX_CAPABILITY_VERSION;
  header.pid = 0; // 0 = change myself
  cap.effective = cap.permitted = (1 << CAP_NET_ADMIN);

  if(capset(&header, &cap) < 0) {
    logmsg(ANDROID_LOG_FATAL,"drop_root/capset failed: %s",strerror(errno));
    exit(1);
  }
}

/* function: open_raw_socket
 * opens the raw socket for sending IPv6 packets
 */
void open_raw_socket(struct tun_data *tunnel) {
  int rawsock = socket(AF_INET6, SOCK_RAW, IPPROTO_RAW);
  if (rawsock < 0) {
    logmsg(ANDROID_LOG_FATAL, "raw socket failed: %s", strerror(errno));
    exit(1);
  }

  int off = 0;
  if (setsockopt(rawsock, SOL_IPV6, IPV6_CHECKSUM, &off, sizeof(off)) < 0) {
    logmsg(ANDROID_LOG_WARN, "could not disable checksum on raw socket: %s", strerror(errno));
  }

  tunnel->write_fd6 = rawsock;
}

/* function: configure_interface
 * reads the configuration and applies it to the interface
 * uplink_interface - network interface to use to reach the ipv6 internet
 * plat_prefix      - PLAT prefix to use
 * tunnel           - tun device data
 * net_id           - NetID to use, NETID_UNSET indicates use of default network
 */
void configure_interface(const char *uplink_interface, const char *plat_prefix, struct tun_data *tunnel, unsigned net_id) {
  int error;

  if(!read_config("/system/etc/clatd.conf", uplink_interface, plat_prefix, net_id)) {
    logmsg(ANDROID_LOG_FATAL,"read_config failed");
    exit(1);
  }

  if(Global_Clatd_Config.mtu > MAXMTU) {
    logmsg(ANDROID_LOG_WARN,"Max MTU is %d, requested %d", MAXMTU, Global_Clatd_Config.mtu);
    Global_Clatd_Config.mtu = MAXMTU;
  }
  if(Global_Clatd_Config.mtu <= 0) {
    Global_Clatd_Config.mtu = getifmtu(Global_Clatd_Config.default_pdp_interface);
    logmsg(ANDROID_LOG_WARN,"ifmtu=%d",Global_Clatd_Config.mtu);
  }
  if(Global_Clatd_Config.mtu < 1280) {
    logmsg(ANDROID_LOG_WARN,"mtu too small = %d", Global_Clatd_Config.mtu);
    Global_Clatd_Config.mtu = 1280;
  }

  if(Global_Clatd_Config.ipv4mtu <= 0 ||
     Global_Clatd_Config.ipv4mtu > Global_Clatd_Config.mtu - MTU_DELTA) {
    Global_Clatd_Config.ipv4mtu = Global_Clatd_Config.mtu - MTU_DELTA;
    logmsg(ANDROID_LOG_WARN,"ipv4mtu now set to = %d",Global_Clatd_Config.ipv4mtu);
  }

  error = tun_alloc(tunnel->device6, tunnel->read_fd6);
  if(error < 0) {
    logmsg(ANDROID_LOG_FATAL,"tun_alloc failed: %s",strerror(errno));
    exit(1);
  }

  error = tun_alloc(tunnel->device4, tunnel->fd4);
  if(error < 0) {
    logmsg(ANDROID_LOG_FATAL,"tun_alloc/4 failed: %s",strerror(errno));
    exit(1);
  }

  configure_tun_ip(tunnel);
}

/* function: read_packet
 * reads a packet from the tunnel fd and passes it down the stack
 * active_fd - tun file descriptor marked ready for reading
 * tunnel    - tun device data
 */
void read_packet(int active_fd, const struct tun_data *tunnel) {
  ssize_t readlen;
  uint8_t packet[PACKETLEN];

  // In case something ignores the packet length.
  // TODO: remove it.
  memset(packet, 0, PACKETLEN);

  readlen = read(active_fd,packet,PACKETLEN);

  if(readlen < 0) {
    logmsg(ANDROID_LOG_WARN,"read_packet/read error: %s", strerror(errno));
    return;
  } else if(readlen == 0) {
    logmsg(ANDROID_LOG_WARN,"read_packet/tun interface removed");
    running = 0;
  } else {
    ssize_t header_size = sizeof(struct tun_pi);

    if(readlen < header_size) {
      logmsg(ANDROID_LOG_WARN,"read_packet/short read: got %ld bytes", readlen);
      return;
    }

    struct tun_pi *tun_header = (struct tun_pi *) packet;
    if(tun_header->flags != 0) {
      logmsg(ANDROID_LOG_WARN, "%s: unexpected flags = %d", __func__, tun_header->flags);
    }

    int fd;
    uint16_t proto = ntohs(tun_header->proto);
    if (proto == ETH_P_IP) {
      fd = tunnel->write_fd6;
    } else if (proto == ETH_P_IPV6) {
      fd = tunnel->fd4;
    } else {
      logmsg(ANDROID_LOG_WARN, "%s: unknown packet type = 0x%x", __func__, proto);
      return;
    }

    translate_packet(fd, (proto == ETH_P_IP), packet + header_size, readlen - header_size);
  }
}

/* function: event_loop
 * reads packets from the tun network interface and passes them down the stack
 * tunnel - tun device data
 */
void event_loop(const struct tun_data *tunnel) {
  time_t last_interface_poll;
  struct pollfd wait_fd[2];

  // start the poll timer
  last_interface_poll = time(NULL);

  wait_fd[0].fd = tunnel->read_fd6;
  wait_fd[0].events = POLLIN;
  wait_fd[0].revents = 0;
  wait_fd[1].fd = tunnel->fd4;
  wait_fd[1].events = POLLIN;
  wait_fd[1].revents = 0;

  while(running) {
    if(poll(wait_fd, 2, NO_TRAFFIC_INTERFACE_POLL_FREQUENCY*1000) == -1) {
      if(errno != EINTR) {
        logmsg(ANDROID_LOG_WARN,"event_loop/poll returned an error: %s",strerror(errno));
      }
    } else {
      int i;
      for(i = 0; i < 2; i++) {
        if((wait_fd[i].revents & POLLIN) != 0) {
          read_packet(wait_fd[i].fd,tunnel);
        }
      }
    }

    time_t now = time(NULL);
    if(last_interface_poll < (now - INTERFACE_POLL_FREQUENCY)) {
      interface_poll(tunnel);
      last_interface_poll = now;
    }
  }
}

/* function: print_help
 * in case the user is running this on the command line
 */
void print_help() {
  printf("android-clat arguments:\n");
  printf("-i [uplink interface]\n");
  printf("-p [plat prefix]\n");
  printf("-n [NetId]\n");
}

/* function: main
 * allocate and setup the tun device, then run the event loop
 */
int main(int argc, char **argv) {
  struct tun_data tunnel;
  int opt;
  char *uplink_interface = NULL, *plat_prefix = NULL, *net_id_str = NULL;
  unsigned net_id = NETID_UNSET;

  strcpy(tunnel.device6, DEVICENAME6);
  strcpy(tunnel.device4, DEVICENAME4);

  while((opt = getopt(argc, argv, "i:p:n:h")) != -1) {
    switch(opt) {
      case 'i':
        uplink_interface = optarg;
        break;
      case 'p':
        plat_prefix = optarg;
        break;
      case 'n':
        net_id_str = optarg;
        break;
      case 'h':
      default:
        print_help();
        exit(1);
        break;
    }
  }

  if(uplink_interface == NULL) {
    logmsg(ANDROID_LOG_FATAL, "clatd called without an interface");
    printf("I need an interface\n");
    exit(1);
  }
  if (net_id_str != NULL) {
    char *end_ptr;
    net_id = strtoul(net_id_str, &end_ptr, 0);
    if (*net_id_str == 0 || *end_ptr != 0) {
      logmsg(ANDROID_LOG_FATAL, "clatd called with invalid NetID %s", net_id_str);
      exit(1);
    }
  }
  logmsg(ANDROID_LOG_INFO, "Starting clat version %s on %s", CLATD_VERSION, uplink_interface);

  // open the tunnel device before dropping privs
  tunnel.read_fd6 = tun_open();
  if(tunnel.read_fd6 < 0) {
    logmsg(ANDROID_LOG_FATAL, "tun_open6 failed: %s", strerror(errno));
    exit(1);
  }

  tunnel.fd4 = tun_open();
  if(tunnel.fd4 < 0) {
    logmsg(ANDROID_LOG_FATAL, "tun_open4 failed: %s", strerror(errno));
    exit(1);
  }

  // open the forwarding configuration before dropping privs
  forwarding_fd = open("/proc/sys/net/ipv6/conf/all/forwarding", O_RDWR);
  if(forwarding_fd < 0) {
    logmsg(ANDROID_LOG_FATAL,"open /proc/sys/net/ipv6/conf/all/forwarding failed: %s",
           strerror(errno));
    exit(1);
  }

  open_raw_socket(&tunnel);

  // run under a regular user
  drop_root();

  // When run from netd, the environment variable ANDROID_DNS_MODE is set to
  // "local", but that only works for the netd process itself.
  unsetenv("ANDROID_DNS_MODE");

  configure_interface(uplink_interface, plat_prefix, &tunnel, net_id);

  set_forwarding(forwarding_fd,"1\n");

  // Loop until someone sends us a signal or brings down the tun interface.
  if(signal(SIGTERM, stop_loop) == SIG_ERR) {
    logmsg(ANDROID_LOG_FATAL, "sigterm handler failed: %s", strerror(errno));
    exit(1);
  }
  event_loop(&tunnel);

  set_forwarding(forwarding_fd,"0\n");
  logmsg(ANDROID_LOG_INFO,"Shutting down clat on %s", uplink_interface);

  return 0;
}
