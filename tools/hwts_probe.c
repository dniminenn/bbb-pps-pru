/* hwts_probe: exercise SIOCSHWTSTAMP + SO_TIMESTAMPING on eth0.
 * udp echo server; prints sw/hw rx stamps and hw tx stamps.
 * hw stamps are raw PHC time (free-running, small seconds).
 */
#include <arpa/inet.h>
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
  int port = argc > 1 ? atoi(argv[1]) : 3123;
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  struct hwtstamp_config hc;
  struct ifreq ifr;
  memset(&hc, 0, sizeof(hc));
  memset(&ifr, 0, sizeof(ifr));
  hc.tx_type = HWTSTAMP_TX_ON;
  hc.rx_filter = HWTSTAMP_FILTER_ALL;
  strcpy(ifr.ifr_name, "eth0");
  ifr.ifr_data = (void *)&hc;
  if (ioctl(fd, SIOCSHWTSTAMP, &ifr))
    perror("SIOCSHWTSTAMP");
  else
    printf("hwtstamp set: tx %d rxfilter %d\n", hc.tx_type, hc.rx_filter);
  int f = SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_TX_HARDWARE |
          SOF_TIMESTAMPING_RAW_HARDWARE | SOF_TIMESTAMPING_RX_SOFTWARE |
          SOF_TIMESTAMPING_SOFTWARE;
  setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &f, sizeof(f));
  struct sockaddr_in sa = {AF_INET, htons(port), {INADDR_ANY}};
  if (bind(fd, (struct sockaddr *)&sa, sizeof(sa))) {
    perror("bind");
    return 1;
  }
  printf("listening :%d\n", port);
  for (;;) {
    struct pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, 5000) <= 0) continue;
    uint8_t buf[512], ctrl[512];
    struct sockaddr_in from;
    struct iovec iov = {buf, sizeof(buf)};
    struct msghdr mh = {&from, sizeof(from), &iov, 1, ctrl, sizeof(ctrl), 0};
    if (p.revents & POLLERR) {
      ssize_t n = recvmsg(fd, &mh, MSG_ERRQUEUE);
      if (n >= 0)
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
          if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_TIMESTAMPING) {
            struct timespec *ts = (struct timespec *)CMSG_DATA(c);
            printf("tx hw %lld.%09ld\n", (long long)ts[2].tv_sec,
                   ts[2].tv_nsec);
          }
      continue;
    }
    ssize_t n = recvmsg(fd, &mh, 0);
    if (n <= 0) continue;
    struct timespec sw = {0, 0}, hw = {0, 0};
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
      if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_TIMESTAMPING) {
        struct timespec *ts = (struct timespec *)CMSG_DATA(c);
        sw = ts[0];
        hw = ts[2];
      }
    printf("rx %zd B sw %lld.%09ld hw %lld.%09ld\n", n, (long long)sw.tv_sec,
           sw.tv_nsec, (long long)hw.tv_sec, hw.tv_nsec);
    mh.msg_control = NULL;
    mh.msg_controllen = 0;
    sendto(fd, buf, n, 0, (struct sockaddr *)&from, sizeof(from));
  }
}
