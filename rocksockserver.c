/*
 * 
 * author: rofl0r
 * 
 * License: LGPL 2.1+ with static linking exception
 * 
 * 
 */


#include <string.h>
#include <stdio.h>
#include <netdb.h>
#include <unistd.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

// rocksock.h only needed for struct rs_hostInfo
#include "rocksock.h"
#include "rocksockserver.h"

#include "endianness.h"

int microsleep(long microsecs) {
	struct timespec req, rem;
	req.tv_sec = microsecs / 1000000;
	req.tv_nsec = (microsecs % 1000000) * 1000;
	int ret;
	while((ret = nanosleep(&req, &rem)) == -1 && errno == EINTR) req = rem;
	return ret;	
}

int rocksockserver_resolve_host(rs_hostInfo* hostinfo) {
	struct addrinfo hints;
	int ret;
	char pbuf[6];
	if (!hostinfo || !hostinfo->host || !hostinfo->port) return -1;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	snprintf(pbuf, 6, "%d", hostinfo->port);
	ret = getaddrinfo(hostinfo->host, pbuf, &hints, &hostinfo->hostaddr);
	if(!ret) {
		return 0;
	} else {
		fprintf(stderr, "error resolving: %s\n", gai_strerror(ret));
		return ret;
	}	
}

int rocksockserver_init(rocksockserver* srv, char* listenip, short port, void* userdata) {
	int ret = 0;
	int yes = 1;
	struct addrinfo* p;
	rs_hostInfo conn;
	if(!srv || !listenip || !port) return -1;
	conn.host = listenip;
	conn.port = port;
	srv->userdata = userdata;
	srv->sleeptime_us = 20000; // set a reasonable default value. it's a compromise between throughput and cpu usage basically.
	ret = rocksockserver_resolve_host(&conn);
	if(ret) return ret;
	for(p = conn.hostaddr; p != NULL; p = p->ai_next) {
		srv->listensocket = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (srv->listensocket < 0) { 
			continue;
		}
		
		// lose the pesky "address already in use" error message
		setsockopt(srv->listensocket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

		if (bind(srv->listensocket, p->ai_addr, p->ai_addrlen) < 0) {
			close(srv->listensocket);
			continue;
		}

		break;
	}
	if (!p) {
		fprintf(stderr, "selectserver: failed to bind\n");
		ret = -1;
	}
	freeaddrinfo(conn.hostaddr);
	// listen
	if (listen(srv->listensocket, 10) == -1) {
		perror("listen");
		ret = -2;
	}	
	return ret;
}

int rocksockserver_disconnect_client(rocksockserver* srv, int client) {
	if(client < 0 || client > USER_MAX_FD) return -1;
	if(FD_ISSET(client, &srv->master)) {
		close(client);
		FD_CLR(client, &srv->master);
		if(client == srv->maxfd)
			srv->maxfd--;
		srv->numfds--;
		return 0;
	}
	return 1;
}

void rocksockserver_set_sleeptime(rocksockserver* srv, long microsecs) {
	srv->sleeptime_us = microsecs;
}

int rocksockserver_loop(rocksockserver* srv, char* buf, size_t bufsize,
			int (*on_clientconnect) (void* userdata, struct sockaddr_storage* clientaddr, int fd), 
			int (*on_clientread) (void* userdata, int fd, size_t nread),
			int (*on_clientwantsdata) (void* userdata, int fd),
			int (*on_clientdisconnect) (void* userdata, int fd)
) {
	fd_set read_fds, write_fds;
	int newfd, i,k;
	int lastfd = 3;
	size_t j;
	ptrdiff_t nbytes;
	struct sockaddr_storage remoteaddr; // client address
	socklen_t addrlen;
	char* fdptr;
	fd_set* setptr;
	
	FD_SET(srv->listensocket, &srv->master);
	srv->maxfd = srv->listensocket;

	for(;;) {
		
		read_fds = srv->master;
		write_fds = srv->master;
		
		if ((srv->numfds = select(srv->maxfd+1, &read_fds, &write_fds, NULL, NULL)) && srv->numfds == -1) 
			perror("select");

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
			puts("hmmz. shouldnt be here");
			printf("maxfd %d, k %d, numfds %d, set %d\n", srv->maxfd, k, srv->numfds, *(int*)(fdptr));
			for(k = 0; k < USER_MAX_FD; k++)
				if(FD_ISSET(k, setptr))
					printf("bit set: %d\n", k);
			abort();
		}
		
		handleread:
		//printf("read_fd %d\n", k);
		if (k == srv->listensocket) {
			// new connection available
			addrlen = sizeof(remoteaddr);
			newfd = accept(srv->listensocket, (struct sockaddr *)&remoteaddr, &addrlen);

			if (newfd == -1) {
				perror("accept");
			} else {
				if(newfd >= USER_MAX_FD) 
					close(newfd); // only USER_MAX_FD connections can be handled.
				else {
					FD_SET(newfd, &srv->master);
					if (newfd > srv->maxfd) 
						srv->maxfd = newfd;
					if(on_clientconnect) on_clientconnect(srv->userdata, &remoteaddr, newfd);
				}	
			}
		} else {
			if ((nbytes = recv(k, buf, bufsize, 0)) <= 0) {
				if (nbytes == 0) {
					if(on_clientdisconnect) on_clientdisconnect(srv->userdata, k);
				} else {
					perror("recv");
				}
				rocksockserver_disconnect_client(srv, k);
			} else {
				if(on_clientread) on_clientread(srv->userdata, k, nbytes);
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