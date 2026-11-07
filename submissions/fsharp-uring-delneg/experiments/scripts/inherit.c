// checks whether TCP_QUICKACK=0 and TCP_THIN_LINEAR_TIMEOUTS set on a listener are inherited by accepted sockets
#include <stdio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#ifndef TCP_THIN_LINEAR_TIMEOUTS
#define TCP_THIN_LINEAR_TIMEOUTS 16
#endif
static int get(int fd, int opt) { int v = -1; socklen_t l = 4; getsockopt(fd, IPPROTO_TCP, opt, &v, &l); return v; }
int main(void) {
  int l = socket(AF_INET, SOCK_STREAM, 0), one = 1, zero = 0;
  struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = 0; a.sin_addr.s_addr = htonl(0x7f000001);
  setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, 4);
  bind(l, (void*)&a, sizeof a); listen(l, 16); socklen_t al = sizeof a; getsockname(l, (void*)&a, &al); setvbuf(stdout, 0, _IONBF, 0);
  setsockopt(l, IPPROTO_TCP, TCP_QUICKACK, &zero, 4);
  setsockopt(l, IPPROTO_TCP, TCP_THIN_LINEAR_TIMEOUTS, &one, 4);
  printf("listener: quickack=%d thin=%d\n", get(l, TCP_QUICKACK), get(l, TCP_THIN_LINEAR_TIMEOUTS));
  int c = socket(AF_INET, SOCK_STREAM, 0); connect(c, (void*)&a, sizeof a);
  int s = accept(l, 0, 0);
  printf("accepted: quickack=%d thin=%d nodelay=%d\n", get(s, TCP_QUICKACK), get(s, TCP_THIN_LINEAR_TIMEOUTS), get(s, TCP_NODELAY));
  return 0;
}
