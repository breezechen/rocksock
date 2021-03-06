/*
 *
 * author: rofl0r
 *
 * License: LGPL 2.1+ with static linking exception
 *
 *
 */

#include <string.h>
#include <stddef.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

#ifndef WIN32
#include <netdb.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#endif // !WIN32

#include "rocksockserver.h"

#include "endianness.h"

#define LOGP(X) do { if(srv->perr) srv->perr(X); } while(0)

#ifdef USE_LIBULZ
#include "../lib/include/strlib.h"
#include "../lib/include/timelib.h"
#else
#include <stdio.h>
#ifndef WIN32
#include <arpa/inet.h>
#define microsleep(X) usleep(X)
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define microsleep(X) Sleep((X)/1000)
#endif

static inline char* my_intToString(int i, char *b, size_t s) {
	int x = snprintf(b, s, "%d", i);
	if(x > 0 && x < s) return b;
	return 0;
}
#define intToString(i, b) my_intToString(i, b, sizeof(b))
#define ipv4fromstring(s, b) inet_aton(s, (struct in_addr *)(void*)(b))
#endif

typedef struct {
	const char* host;
	unsigned short port;
#ifndef IPV4_ONLY
	struct addrinfo* hostaddr;
#else
	struct sockaddr_in hostaddr;
#endif
} rs_hostInfo;

int rocksockserver_resolve_host(rs_hostInfo* hostinfo) {
	if (!hostinfo || !hostinfo->host || !hostinfo->port) return -1;
#ifndef IPV4_ONLY
	char pbuf[8];
	char* ports;
	int ret;
	struct addrinfo hints;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	if(!(ports = intToString(hostinfo->port, pbuf))) return -1;
	return getaddrinfo(hostinfo->host, ports, &hints, &hostinfo->hostaddr);
#else
	memset(&hostinfo->hostaddr, 0, sizeof(struct sockaddr_in));
	ipv4fromstring(hostinfo->host, (unsigned char*) &hostinfo->hostaddr.sin_addr);
	hostinfo->hostaddr.sin_family = AF_INET;
	hostinfo->hostaddr.sin_port = htons(hostinfo->port);
	return 0;
#endif
}

/* returns 0 on success.
   possible error return codes:
   -1: erroneus parameter
   -2: bind() failed
   -3: socket() failed
   -4: listen() failed
   positive number: dns error, pass to gai_strerror()
*/
int rocksockserver_init(rocksockserver* srv, const char* listenip, unsigned short port, void* userdata) {
	int ret = 0;
	int yes = 1;
	rs_hostInfo conn;
	if(!srv || !listenip || !port) return -1;
	conn.host = listenip;
	conn.port = port;
	FD_ZERO(&srv->master);
	srv->userdata = userdata;
	srv->sleeptime_us = 20000; // set a reasonable default value. it's a compromise between throughput and cpu usage basically.
	ret = rocksockserver_resolve_host(&conn);
	if(ret) return ret;
#ifndef IPV4_ONLY
	struct addrinfo* p;
	for(p = conn.hostaddr; p != NULL; p = p->ai_next) {
		srv->listensocket = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (srv->listensocket < 0) {
			continue;
		}

		// lose the pesky "address already in use" error message
		setsockopt(srv->listensocket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

		if (bind(srv->listensocket, p->ai_addr, p->ai_addrlen) < 0) {
#ifdef WIN32
			closesocket(srv->listensocket);
#else
			close(srv->listensocket);
#endif
			continue;
		}

		break;
	}
	if (!p) {
		LOGP("bind");
		ret = -2;
	}
	freeaddrinfo(conn.hostaddr);
	if(ret == -2) return -2;
#else
	srv->listensocket = socket(AF_INET, SOCK_STREAM, 0);
	if(srv->listensocket < 0) {
		LOGP("socket");
		return -3;
	}
	setsockopt(srv->listensocket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));
	if(bind(srv->listensocket, (struct sockaddr*) &conn.hostaddr, sizeof(struct sockaddr_in)) < 0) {
		close(srv->listensocket);
		LOGP("bind");
		return -2;
	}

#endif
	// listen
	if (listen(srv->listensocket, 10) == -1) {
		LOGP("listen");
		ret = -4;
	} else {
		FD_SET(srv->listensocket, &srv->master);
		srv->maxfd = srv->listensocket;
	}
	return ret;
}

int rocksockserver_disconnect_client(rocksockserver* srv, int client) {
	if(client < 0 || client > USER_MAX_FD) return -1;
	if(FD_ISSET(client, &srv->master)) {
#ifdef WIN32
		closesocket(client);
#else
		close(client);
#endif
		FD_CLR(client, &srv->master);
		if(client == srv->maxfd)
			srv->maxfd--;
		srv->numfds--;
		return 0;
	}
	return 1;
}

void rocksockserver_watch_fd(rocksockserver* srv, int newfd) {
	FD_SET(newfd, &srv->master);
	if (newfd > srv->maxfd)
		srv->maxfd = newfd;
}

int rocksockserver_loop(rocksockserver* srv,
			char* buf, size_t bufsize,
			int (*on_clientconnect) (void* userdata, struct sockaddr_storage* clientaddr, int fd),
			int (*on_clientread) (void* userdata, int fd, size_t nread),
			int (*on_clientwantsdata) (void* userdata, int fd),
			int (*on_clientdisconnect) (void* userdata, int fd)
) {
	fd_set read_fds, write_fds;
	int newfd, k;
	int lastfd = 3;
#ifdef IS_LITTLE_ENDIAN
	int i;
	size_t j;
#endif
	ptrdiff_t nbytes;
	struct sockaddr_storage remoteaddr; // client address
	socklen_t addrlen;
	char* fdptr;
	fd_set* setptr;

	for(;;) {

		read_fds = srv->master;
		write_fds = srv->master;

		if ((srv->numfds = select(srv->maxfd+1, &read_fds, &write_fds, NULL, NULL)) && srv->numfds == -1)
			LOGP("select");

		if(!srv->numfds) continue;

		// optimization for the case searched_fd = lastfd, when we only have to handle one connection.
		// i guess that should be the majority of cases.
		k = lastfd;
		setptr = &write_fds;
		if(FD_ISSET(k, setptr)) goto gotcha;
		setptr = &read_fds;
		if(FD_ISSET(k, setptr)) goto gotcha;

		nextfd:
		setptr = &write_fds;
		loopstart:
		fdptr = (char*) setptr;
#ifdef IS_LITTLE_ENDIAN
		for(i = 0; i * CHAR_BIT <= srv->maxfd; i+= sizeof(size_t)) { // we assume that sizeof(fd_set) is a multiple of sizeof(size_t)
			if( *(size_t*)(fdptr + i)) {
				for(j = 0; j <= sizeof(size_t); j++) {
					if(fdptr[i + j]) {
						for(k = (i + j) * CHAR_BIT; k <= srv->maxfd; k++) {
#else
						for(k = 0; k <= srv->maxfd; k++) {
#endif
							if(FD_ISSET(k, setptr)) {
								gotcha:
								srv->numfds--;
								FD_CLR(k, setptr);
								if(setptr == &write_fds)
									goto handlewrite;
								else
									goto handleread;
							}
						}
#ifdef IS_LITTLE_ENDIAN
					}
				}
			}
		}

#endif

		if(setptr == &write_fds) {
			setptr = &read_fds;
			goto loopstart;
		} else {
			LOGP("FATAL");
			/*
			printf("maxfd %d, k %d, numfds %d, set %d\n", srv->maxfd, k, srv->numfds, *(int*)(fdptr));
			for(k = 0; k < USER_MAX_FD; k++)
				if(FD_ISSET(k, setptr))
					printf("bit set: %d\n", k);
			*/
			return 1;
		}

		handleread:
		//printf("read_fd %d\n", k);
		if (k == srv->listensocket) {
			// new connection available
			addrlen = sizeof(remoteaddr);
			newfd = accept(srv->listensocket, (struct sockaddr *)&remoteaddr, &addrlen);

			if (newfd == -1) {
				LOGP("accept");
			} else {
				if (newfd >= USER_MAX_FD)
#ifdef WIN32
					closesocket(newfd);
#else
					close(newfd); // only USER_MAX_FD connections can be handled.
#endif
				else {
					FD_SET(newfd, &srv->master);
					if (newfd > srv->maxfd)
						srv->maxfd = newfd;
					if(on_clientconnect) on_clientconnect(srv->userdata, &remoteaddr, newfd);
				}
			}
		} else {
			if(buf && k != srv->signalfd) {
				if ((nbytes = recv(k, buf, bufsize, 0)) <= 0) {
					if (nbytes == 0) {
						if(on_clientdisconnect) on_clientdisconnect(srv->userdata, k);
					} else {
						LOGP("recv");
					}
					rocksockserver_disconnect_client(srv, k);
				} else {
					if(on_clientread) on_clientread(srv->userdata, k, nbytes);
				}
			} else {

				if(on_clientread) on_clientread(srv->userdata, k, 0);
			}
		}
		goto zzz;

		handlewrite:

		//printf("write_fd %d\n", k);
		if(on_clientwantsdata) on_clientwantsdata(srv->userdata, k);

		zzz:
		if(srv->numfds > 0) goto nextfd;
		lastfd = k;
		microsleep(srv->sleeptime_us);
	}
	return 0;
}
