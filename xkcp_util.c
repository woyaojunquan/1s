/********************************************************************\
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
 *                                                                  *
 * This program is distributed in the hope that it will be useful,  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of   *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the    *
 * GNU General Public License for more details.                     *
 *                                                                  *
 * You should have received a copy of the GNU General Public License*
 * along with this program; if not, contact:                        *
 *                                                                  *
 * Free Software Foundation           Voice:  +1-617-542-5942       *
 * 59 Temple Place - Suite 330        Fax:    +1-617-542-2652       *
 * Boston, MA  02111-1307,  USA       gnu@gnu.org                   *
 *                                                                  *
\********************************************************************/


#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <arpa/inet.h>
#include <net/if.h>

#include <event2/event.h>
#include <event2/event_struct.h>
#include <event2/bufferevent_ssl.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>

#include <syslog.h>

#include "ikcp.h"
#include "xkcp_util.h"
#include "xkcp_config.h"
#include "commandline.h"
#include "debug.h"

static int task_list_count = 0;

int get_task_list_count()
{
	return task_list_count;
}

void itimeofday(long *sec, long *usec)
{
	struct timeval time;
	gettimeofday(&time, NULL);
	if (sec) *sec = time.tv_sec;
	if (usec) *usec = time.tv_usec;
}

/* get clock in millisecond 64 */
IINT64 iclock64(void)
{
	long s, u;
	IINT64 value;
	itimeofday(&s, &u);
	value = ((IINT64)s) * 1000 + (u / 1000);
	return value;
}

IUINT32 iclock()
{
	return (IUINT32)(iclock64() & 0xfffffffful);
}

char *get_iface_ip(const char *ifname)
{
    struct ifreq if_data;
    struct in_addr in;
    char *ip_str;
    int sockd;
    u_int32_t ip;

    /* Create a socket */
    if ((sockd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        debug(LOG_ERR, "socket(): %s", strerror(errno));
        return NULL;
    }

    /* Get IP of internal interface */
    strncpy(if_data.ifr_name, ifname, 15);
    if_data.ifr_name[15] = '\0';

    /* Get the IP address */
    if (ioctl(sockd, SIOCGIFADDR, &if_data) < 0) {
        debug(LOG_ERR, "ioctl(): SIOCGIFADDR %s", strerror(errno));
        close(sockd);
        return NULL;
    }
    memcpy((void *)&ip, (void *)&if_data.ifr_addr.sa_data + 2, 4);
    in.s_addr = ip;
	
    close(sockd);
	ip_str = malloc(HTTP_IP_ADDR_LEN);
	memset(ip_str, 0, HTTP_IP_ADDR_LEN);
	if(ip_str&&inet_ntop(AF_INET, &in, ip_str, HTTP_IP_ADDR_LEN))
    	return ip_str;
	
	if (ip_str) free(ip_str);	
	return NULL;
}

static void
__list_add(iqueue_head *entry, iqueue_head *prev, iqueue_head *next)
{
    next->prev = entry;
    entry->next = next;
    entry->prev = prev;
    prev->next = entry;
}

static void
__list_del(iqueue_head *prev, iqueue_head *next)
{
    next->prev = prev;
    prev->next = next;
}

void 
add_task_tail(struct xkcp_task *task, iqueue_head *head) { 
	iqueue_head *entry = &task->head;	
	
	__list_add(entry, head->prev, head);	
}

void
del_task(struct xkcp_task *task) {
	iqueue_head *entry = &task->head;
	
	__list_del(entry->prev, entry->next);	
}

static int xkcp_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
	struct xkcp_proxy_param *ptr = user;
    int nret = sendto(ptr->udp_fd, buf, len, 0, (struct sockaddr *)&ptr->serveraddr, sizeof(ptr->serveraddr));
	if (nret > 0)
    	debug(LOG_DEBUG, "xkcp_output conv [%d] fd [%d] len [%d], send datagram from %d",
          	kcp->conv, ptr->udp_fd, len, nret);
	else
		debug(LOG_INFO, "xkcp_output conv [%d] fd [%d] send datagram error: (%s)",
          	kcp->conv, ptr->udp_fd, strerror(errno));
	
    return nret;
}

void xkcp_set_config_param(ikcpcb *kcp)
{
	struct xkcp_param *param = xkcp_get_param();
	kcp->output	= xkcp_output;
	ikcp_wndsize(kcp, param->sndwnd, param->rcvwnd);
	ikcp_nodelay(kcp, param->nodelay, param->interval, param->resend, param->nc);
	debug(LOG_DEBUG, "sndwnd [%d] rcvwnd [%d] nodelay [%d] interval [%d] resend [%d] nc [%d]",
		 param->sndwnd, param->rcvwnd, param->nodelay, param->interval, param->resend, param->nc);
}

void xkcp_tcp_event_cb(struct bufferevent *bev, short what, struct xkcp_task *task)
{
	if (what & (BEV_EVENT_EOF|BEV_EVENT_ERROR)) {
		if (task) {
			debug(LOG_INFO, "tcp_client_event_cb what is [%d] socket [%d]", 
				  what, bufferevent_getfd(bev));
			if (task->bev != bev) {
				bufferevent_free(task->bev);
				debug(LOG_ERR, "impossible here\n");
			}
			ikcp_flush(task->kcp);
			del_task(task);
			free(task);
		}
		bufferevent_free(bev);
	}
}

void xkcp_tcp_read_cb(struct bufferevent *bev, ikcpcb *kcp)
{
#if	0
	struct evbuffer *src;
	size_t	len;

	src = bufferevent_get_input(bev);
	len = evbuffer_get_length(src);

	if (len > 0) {
		char *data = malloc(len);
		memset(data, 0, len);
		evbuffer_copyout(src, data, len);
		evbuffer_drain(src, len);
		debug(LOG_INFO, "conv [%d] read data from client [%d]", kcp->conv, len);
		ikcp_send(kcp, data, len);
		free(data);
	}
#else
	char buf[1024] = {0};
    int  len;
    struct evbuffer *input = bufferevent_get_input(bev);
    while ((len = evbuffer_remove(input, buf, sizeof(buf))) > 0) {
        debug(LOG_INFO, "conv [%d] read data from client [%d]", kcp->conv, len);
		ikcp_send(kcp, data, len);
    }
#endif
}


void xkcp_forward_all_data(iqueue_head *task_list)
{
	struct xkcp_task *task;
	iqueue_foreach(task, task_list, xkcp_task_type, head) {
		if (task->kcp) {
			xkcp_forward_data(task);
		}
	}
}

void xkcp_forward_data(struct xkcp_task *task)
{
	while(1) {
		char obuf[OBUF_SIZE] = {0};
		int nrecv = ikcp_recv(task->kcp, obuf, OBUF_SIZE);
		if (nrecv < 0) {
			if (nrecv == -3)
				debug(LOG_INFO, "obuf is small, need to extend it");
			break;
		}

		debug(LOG_INFO, "xkcp_forward_data conv [%d] send [%d]", task->kcp->conv, nrecv);
		if (task->bev)
			evbuffer_add(bufferevent_get_output(task->bev), obuf, nrecv);
		else
			debug(LOG_INFO, "this task has finished");
	}
}


ikcpcb *
get_kcp_from_conv(int conv, iqueue_head *task_list)
{
	struct xkcp_task *task;
	iqueue_foreach(task, task_list, xkcp_task_type, head) 
		if (task->kcp && task->kcp->conv == conv)
			return task->kcp;

	return NULL;
}

int xkcp_main(int argc, char **argv)
{
	struct xkcp_config *config = xkcp_get_config();

	config_init();

	parse_commandline(argc, argv);

	if ( xkcp_parse_param(config->config_file) ) {
		debug(LOG_ERR, "xkcp_parse_param failed \n");
		usage();
		exit(0);
	}
	
	if (config->main_loop == NULL) {
		debug(LOG_ERR, "should set main_loop firstly");
		exit(0);
	}
	
	if (config->daemon) {

        debug(LOG_INFO, "Forking into background");

        switch (fork()) {
        case 0:                /* child */
            setsid();
            config->main_loop();
            break;

        default:               /* parent */
            exit(0);
            break;
        }
    } else {
        config->main_loop();
    }

    return (0);                 /* never reached */
}

void
set_timer_interval(struct event *timeout)
{
	struct timeval tv;
	evutil_timerclear(&tv);
    tv.tv_usec = xkcp_get_param()->interval;
	event_add(timeout, &tv);
}

void xkcp_timer_event_cb(struct event *timeout, iqueue_head *task_list)
{
	struct xkcp_task *task;
	task_list_count = 0;
	iqueue_foreach(task, task_list, xkcp_task_type, head) {
		if (task->kcp) {
			ikcp_update(task->kcp, iclock());	
			task_list_count++;
		}
	}
	
	set_timer_interval(timeout);
}
