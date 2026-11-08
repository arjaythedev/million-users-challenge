/* OS ABI adapters only; HTTP, scheduling and application logic live in BASIC. */
#define _GNU_SOURCE
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <jansson.h>
int basic_listener(const char *host,int port) {
 int s=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0), one=1;
 if(s<0)return -1;
 setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
 setsockopt(s,SOL_SOCKET,SO_REUSEPORT,&one,sizeof(one));
 struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(port)};
 if(inet_pton(AF_INET,host,&a.sin_addr)!=1 || bind(s,(void*)&a,sizeof(a)) || listen(s,65535)) {close(s);return -1;} return s;
}
int basic_accept(int s){return accept4(s,0,0,SOCK_NONBLOCK|SOCK_CLOEXEC);}
int basic_errno(void){return errno;}
void basic_release(json_t *j){json_decref(j);}
void basic_signals(void){signal(SIGPIPE,SIG_IGN);prctl(PR_SET_PDEATHSIG,SIGTERM);}
int basic_cpus(void){return sysconf(_SC_NPROCESSORS_ONLN);}
