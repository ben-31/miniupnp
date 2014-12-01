/* $Id: miniupnpc-libevent.c,v 1.18 2014/12/01 17:41:11 nanard Exp $ */
/* miniupnpc-libevent
 * Copyright (c) 2008-2014, Thomas BERNARD <miniupnp@free.fr>
 * http://miniupnp.free.fr/ or http://miniupnp.tuxfamily.org/
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. */
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <stdio.h>
#include <event2/event.h>
#include <event2/buffer.h>
/*#include <event2/bufferevent.h>*/
#include <event2/http.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#define PRINT_SOCKET_ERROR printf
#define SOCKET_ERROR GetWSALastError()
#define WOULDBLOCK(err) (err == WSAEWOULDBLOCK)
#else /* _WIN32 */
#include <unistd.h>
#include <errno.h>
#define closesocket close
#define PRINT_SOCKET_ERROR perror
#define SOCKET_ERROR errno
#define WOULDBLOCK(err) (err == EAGAIN || err == EWOULDBLOCK)
#endif /* _WIN32 */
#include "miniupnpc-libevent.h"
#include "minixml.h"
#include "igd_desc_parse.h"
#include "upnpreplyparse.h"

#ifndef MIN
#define MIN(x,y) (((x)<(y))?(x):(y))
#endif /* MIN */

#ifndef MAXHOSTNAMELEN
#define MAXHOSTNAMELEN 64
#endif /* MAXHOSTNAMELEN */

#define SSDP_PORT 1900
#define SSDP_MCAST_ADDR "239.255.255.250"
#define XSTR(s) STR(s)
#define STR(s) #s

#ifdef DEBUG
#define debug_printf(...) fprintf(stderr, __VA_ARGS__)
#else
#define debug_printf(...)
#endif

/* compare the begining of a string with a constant string */
#define COMPARE(str, cstr) (0==memcmp(str, cstr, sizeof(cstr) - 1))

/* stuctures */

struct upnp_args {
	const char * elt;
	const char * val;
};

/* private functions */

static int upnpc_get_desc(upnpc_device_t * p, const char * url);
static char * build_url_string(const char * urlbase, const char * root_desc_url, const char * controlurl);

/* data */
static const char * devices_to_search[] = {
	"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
	"urn:schemas-upnp-org:service:WANIPConnection:1",
	"urn:schemas-upnp-org:service:WANPPPConnection:1",
	"upnp:rootdevice",
	0
};

#ifdef DEBUG
static void upnpc_conn_close_cb(struct evhttp_connection * conn, void * data)
{
	upnpc_device_t * d = (upnpc_device_t *)data;
	debug_printf("upnpc_get_desc_conn_close_cb %p %p\n", conn, d);
}
#endif /* DEBUG */

/* parse_msearch_reply()
 * the last 4 arguments are filled during the parsing :
 *    - location/locationsize : "location:" field of the SSDP reply packet
 *    - st/stsize : "st:" field of the SSDP reply packet.
 * The strings are NOT null terminated */
static void
parse_msearch_reply(const char * reply, int size,
                    const char * * location, int * locationsize,
                    const char * * st, int * stsize)
{
	int a, b, i;
	i = 0;	/* current character index */
	a = i;	/* start of the line */
	b = 0;	/* end of the "header" (position of the colon) */
	while(i<size) {
		switch(reply[i]) {
		case ':':
			if(b==0) {
				b = i; /* end of the "header" */
			}
			break;
		case '\x0a':
		case '\x0d':
			if(b!=0) {
				/* skip the colon and white spaces */
				do { b++; } while(reply[b]==' ' && b<size);
				if(0==strncasecmp(reply+a, "location", 8)) {
					*location = reply+b;
					*locationsize = i-b;
				} else if(0==strncasecmp(reply+a, "st", 2)) {
					*st = reply+b;
					*stsize = i-b;
				}
				b = 0;
			}
			a = i+1;
			break;
		default:
			break;
		}
		i++;
	}
}

static void upnpc_send_ssdp_msearch(evutil_socket_t s, short events, upnpc_t * p)
{
	/* envoyer les packets de M-SEARCH discovery sur le socket ssdp */
	int n;
	char bufr[1024];
	struct sockaddr_in addr;
	unsigned int mx = 2;
	static const char MSearchMsgFmt[] = 
	"M-SEARCH * HTTP/1.1\r\n"
	"HOST: " SSDP_MCAST_ADDR ":" XSTR(SSDP_PORT) "\r\n"
	"ST: %s\r\n"
	"MAN: \"ssdp:discover\"\r\n"
	"MX: %u\r\n"
	"\r\n";
	(void)p;
	(void)events;

	memset(&addr, 0, sizeof(struct sockaddr_in));
	addr.sin_family = AF_INET;
    addr.sin_port = htons(SSDP_PORT);
    addr.sin_addr.s_addr = inet_addr(SSDP_MCAST_ADDR);
	n = snprintf(bufr, sizeof(bufr),
	             MSearchMsgFmt, devices_to_search[p->discover_device_index++], mx);
	debug_printf("upnpc_send_ssdp_msearch: %s", bufr);
	n = sendto(s, bufr, n, 0,
	           (struct sockaddr *)&addr, sizeof(struct sockaddr_in));
	if (n < 0) {
		PRINT_SOCKET_ERROR("sendto");
	}
}

static int upnpc_set_root_desc_location(upnpc_device_t * d, const char * location, int locationsize)
{
	char * tmp;
	tmp = realloc(d->root_desc_location, locationsize + 1);
	if(tmp == NULL) {
		return -1;
	}
	memcpy(tmp, location, locationsize);
	tmp[locationsize] = '\0';
	d->root_desc_location = tmp;
	return 0;
}

static upnpc_device_t * upnpc_find_device_with_location(upnpc_t * p, const char * location, int locationsize)
{
	upnpc_device_t * d;
	for(d = p->devices; d != NULL; d = d->next) {
		if(d->root_desc_location
		  && ((int)strlen(d->root_desc_location) == locationsize)
		  && (0 == memcmp(location, d->root_desc_location, locationsize)))
			return d;
	}
	return NULL;
}

static void upnpc_receive_and_parse_ssdp(evutil_socket_t s, short events, upnpc_t * p)
{
	char bufr[2048];
	ssize_t len;

	if(events == EV_TIMEOUT) {
		/* nothing received ... */
		debug_printf("upnpc_receive_and_parse_ssdp() TIMEOUT\n");
		if(!devices_to_search[p->discover_device_index]) {
			debug_printf("*** NO MORE DEVICES TO SEARCH ***\n");
			event_del(p->ev_ssdp_recv);
			/* no device found : report error */
			p->ready_cb(UPNPC_ERR_NO_DEVICE_FOUND, p, NULL, p->cb_data);
		} else {
			/* send another SSDP M-SEARCH packet */
			if(event_add(p->ev_ssdp_writable, NULL)) {
				debug_printf("event_add FAILED\n");
			}
		}
		return;
	}
	len = recv(s, bufr, sizeof(bufr), 0);
	debug_printf("input %d bytes\n", (int)len);
	if(len < 0) {
		PRINT_SOCKET_ERROR("recv");
	} else if(len == 0) {
		debug_printf("SSDP socket closed ?\n");
	} else {
		const char * location = NULL;
		int locationsize;
		const char * st = NULL;
		int stsize;
		debug_printf("%.*s", (int)len, bufr);
		parse_msearch_reply(bufr, len, &location, &locationsize, &st, &stsize);
		debug_printf("location = '%.*s'\n", locationsize, location);
		debug_printf("st = '%.*s'\n", stsize, st);
		if(location != NULL) {
			upnpc_device_t * device;
			device = upnpc_find_device_with_location(p, location, locationsize);
			if(device) {
				debug_printf("device already known\n");
			} else {
				device = malloc(sizeof(upnpc_device_t));
				memset(device, 0, sizeof(upnpc_device_t));
				device->parent = p;
				device->next = p->devices;
				p->devices = device;
				if(upnpc_set_root_desc_location(device, location, locationsize) < 0) {
					return;
				}
				upnpc_get_desc(device, device->root_desc_location);
			}
#if 0
			event_del(p->ev_ssdp_recv);	/* stop receiving SSDP responses */
#endif
		} else {
			/* or do nothing ? */
			debug_printf("no location\n");
		}
	}
}

static int
parseURL(const char * url,
         char * hostname, unsigned short * port,
         char * * path, unsigned int * scope_id)
{
	char * p1, *p2, *p3;
	if(!url)
		return 0;
	p1 = strstr(url, "://");
	if(!p1)
		return 0;
	p1 += 3;
	if(  (url[0]!='h') || (url[1]!='t')
	   ||(url[2]!='t') || (url[3]!='p'))
		return 0;
	memset(hostname, 0, MAXHOSTNAMELEN + 1);
	if(*p1 == '[') {
		/* IP v6 : http://[2a00:1450:8002::6a]/path/abc */
		char * scope;
		scope = strchr(p1, '%');
		p2 = strchr(p1, ']');
		if(p2 && scope && scope < p2 && scope_id) {
			/* parse scope */
#ifdef IF_NAMESIZE
			char tmp[IF_NAMESIZE];
			int l;
			scope++;
			/* "%25" is just '%' in URL encoding */
			if(scope[0] == '2' && scope[1] == '5')
				scope += 2;	/* skip "25" */
			l = p2 - scope;
			if(l >= IF_NAMESIZE)
				l = IF_NAMESIZE - 1;
			memcpy(tmp, scope, l);
			tmp[l] = '\0';
			*scope_id = if_nametoindex(tmp);
			if(*scope_id == 0) {
				*scope_id = (unsigned int)strtoul(tmp, NULL, 10);
			}
#else /* IF_NAMESIZE */
			/* under windows, scope is numerical */
			char tmp[8];
			int l;
			scope++;
			/* "%25" is just '%' in URL encoding */
			if(scope[0] == '2' && scope[1] == '5')
				scope += 2;	/* skip "25" */
			l = p2 - scope;
			if(l >= (int)sizeof(tmp))
				l = sizeof(tmp) - 1;
			memcpy(tmp, scope, l);
			tmp[l] = '\0';
			*scope_id = (unsigned int)strtoul(tmp, NULL, 10);
#endif /* IF_NAMESIZE */
		}
		p3 = strchr(p1, '/');
		if(p2 && p3) {
			p2++;
			strncpy(hostname, p1, MIN(MAXHOSTNAMELEN, (int)(p2-p1)));
			if(*p2 == ':') {
				*port = 0;
				p2++;
				while( (*p2 >= '0') && (*p2 <= '9')) {
					*port *= 10;
					*port += (unsigned short)(*p2 - '0');
					p2++;
				}
			} else {
				*port = 80;
			}
			*path = p3;
			return 1;
		}
	}
	p2 = strchr(p1, ':');
	p3 = strchr(p1, '/');
	if(!p3)
		return 0;
	if(!p2 || (p2>p3)) {
		strncpy(hostname, p1, MIN(MAXHOSTNAMELEN, (int)(p3-p1)));
		*port = 80;
	} else {
		strncpy(hostname, p1, MIN(MAXHOSTNAMELEN, (int)(p2-p1)));
		*port = 0;
		p2++;
		while( (*p2 >= '0') && (*p2 <= '9')) {
			*port *= 10;
			*port += (unsigned short)(*p2 - '0');
			p2++;
		}
	}
	*path = p3;
	return 1;
}

static void upnpc_desc_received(struct evhttp_request * req, void * pvoid)
{
	size_t len;
	unsigned char * data;
	struct evbuffer * input_buffer;
	struct IGDdatas igd;
	struct xmlparser parser;
	upnpc_device_t * d = (upnpc_device_t *)pvoid;

	input_buffer = evhttp_request_get_input_buffer(req);
	len = evbuffer_get_length(input_buffer);
	data = evbuffer_pullup(input_buffer, len);
	debug_printf("upnpc_desc_received %d (%d bytes)\n", evhttp_request_get_response_code(req), (int)len);
	if(evhttp_request_get_response_code(req) != HTTP_OK) {
		d->parent->ready_cb(evhttp_request_get_response_code(req), d->parent, d, d->parent->cb_data);
		return;
	}
	if(data == NULL) {
		d->parent->ready_cb(UPNPC_ERR_ROOT_DESC_ERROR, d->parent, d, d->parent->cb_data);
		return;
	}
	debug_printf("%.*s\n", (int)len, (char *)data);

	memset(&igd, 0, sizeof(struct IGDdatas));
	memset(&parser, 0, sizeof(struct xmlparser));
	parser.xmlstart = (char *)data;
	parser.xmlsize = len;
	parser.data = &igd;
	parser.starteltfunc = IGDstartelt;
	parser.endeltfunc = IGDendelt;
	parser.datafunc = IGDdata;
	parsexml(&parser);
#ifdef DEBUG
	printIGD(&igd);
#endif /* DEBUG */
	d->control_conn_url = build_url_string(igd.urlbase, d->root_desc_location, igd.first.controlurl);
	d->conn_service_type = strdup(igd.first.servicetype);
	d->control_cif_url = build_url_string(igd.urlbase, d->root_desc_location, igd.CIF.controlurl);
	d->cif_service_type = strdup(igd.CIF.servicetype);
	debug_printf("control_conn_url='%s'\n  (service_type='%s')\n",
	             d->control_conn_url, d->conn_service_type);
	debug_printf("control_cif_url='%s'\n  (service_type='%s')\n",
	             d->control_cif_url, d->cif_service_type);

	if((d->cif_service_type == NULL)
	  || (strlen(d->cif_service_type) == 0)
	  || (!COMPARE(d->cif_service_type, "urn:schemas-upnp-org:service:WANCommonInterfaceConfig:"))) {
		d->parent->ready_cb(UPNPC_ERR_NOT_IGD, d->parent, d, d->parent->cb_data);
	} else {
		d->state |= UPNPC_DEVICE_GETSTATUS;
		upnpc_get_status_info(d);
	}
}

static void upnpc_soap_response(struct evhttp_request * req, void * pvoid)
{
	size_t len;
	unsigned char * data;
	struct evbuffer * input_buffer;
	upnpc_device_t * d = (upnpc_device_t *)pvoid;
	int code = evhttp_request_get_response_code(req);

	input_buffer = evhttp_request_get_input_buffer(req);
	len = evbuffer_get_length(input_buffer);
	data = evbuffer_pullup(input_buffer, len);
	debug_printf("upnpc_soap_response %d (%d bytes)\n", code, (int)len);
	debug_printf("%.*s\n", (int)len, (char *)data);
	if(data == NULL)
		return;

	ClearNameValueList(&d->soap_response_data);
	ParseNameValue((char *)data, (int)len, 
	               &d->soap_response_data);
	if(d->state & UPNPC_DEVICE_READY) {
		d->parent->soap_cb(code, d->parent, d, d->parent->cb_data);
	} else if(d->state & UPNPC_DEVICE_GETSTATUS) {
		const char * connection_status;
		d->state &= ~UPNPC_DEVICE_GETSTATUS;
		connection_status = GetValueFromNameValueList(&d->soap_response_data, "NewConnectionStatus");
		d->state |= UPNPC_DEVICE_READY;
		if((code == 200) && connection_status && (0 == strcmp("Connected", connection_status))) {
			d->parent->ready_cb(code, d->parent, d, d->parent->cb_data);
			d->state |= UPNPC_DEVICE_CONNECTED;
			event_del(d->parent->ev_ssdp_recv);
		} else {
			d->parent->ready_cb(UPNPC_ERR_NOT_CONNECTED, d->parent, d, d->parent->cb_data);
		}
	}
	d->state &= ~UPNPC_DEVICE_SOAP_REQ;
}

static int upnpc_get_desc(upnpc_device_t * d, const char * url)
{
	char hostname[MAXHOSTNAMELEN+1];
	unsigned short port;
	char * path;
	unsigned int scope_id;
	struct evhttp_request * req;
	struct evkeyvalq * headers;

	/* if(d->root_desc_location == NULL) {
		return -1;
	} */
	if(!parseURL(url/*d->root_desc_location*/, hostname, &port,
	             &path, &scope_id)) {
		return -1;
	}
	if(d->desc_conn == NULL) {
		d->desc_conn = evhttp_connection_base_new(d->parent->base, NULL, hostname, port);
	}
#ifdef DEBUG
	evhttp_connection_set_closecb(d->desc_conn, upnpc_conn_close_cb, d);
#endif /* DEBUG */
	/*evhttp_connection_set_timeout(p->desc_conn, 600);*/
	req = evhttp_request_new(upnpc_desc_received/*callback*/, d);
	headers = evhttp_request_get_output_headers(req);
	evhttp_add_header(headers, "Host", hostname);
	evhttp_add_header(headers, "Connection", "close");
	/*evhttp_add_header(headers, "User-Agent", "***");*/
	evhttp_make_request(d->desc_conn, req, EVHTTP_REQ_GET, path);
	return 0;
}

static char * build_url_string(const char * urlbase, const char * root_desc_url, const char * controlurl)
{
	int l, n;
	char * s;
	const char * base;
	char * p;
	/* if controlurl is an absolute url, return it */
	if(0 == memcmp("http://", controlurl, 7))
		return strdup(controlurl);
	base = (urlbase[0] == '\0') ? root_desc_url : urlbase;
	n = strlen(base);
	if(n > 7) {
		p = strchr(base + 7, '/');
		if(p)
			n = p - base;
	}
	l = n + strlen(controlurl) + 1;
	if(controlurl[0] != '/')
		l++;
	s = malloc(l);
	if(s == NULL) return NULL;
	memcpy(s, base, n);
	if(controlurl[0] != '/')
		s[n++] = '/';
	memcpy(s + n, controlurl, l - n);
	return s;
}

#define SOAPPREFIX "s"
#define SERVICEPREFIX "u"
#define SERVICEPREFIX2 'u'

static int upnpc_send_soap_request(upnpc_device_t * p, const char * url,
                                   const char * service,
                                   const char * method,
                                   const struct upnp_args * args, int arg_count)
{
	char action[128];
	char * body;
	const char fmt_soap[] = 
		"<?xml version=\"1.0\"?>\r\n"
		"<" SOAPPREFIX ":Envelope "
		"xmlns:" SOAPPREFIX "=\"http://schemas.xmlsoap.org/soap/envelope/\" "
		SOAPPREFIX ":encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
		"<" SOAPPREFIX ":Body>"
		"<" SERVICEPREFIX ":%s xmlns:" SERVICEPREFIX "=\"%s\">"
		"%s"
		"</" SERVICEPREFIX ":%s>"
		"</" SOAPPREFIX ":Body></" SOAPPREFIX ":Envelope>"
		"\r\n";
	int body_len;
	char hostname[MAXHOSTNAMELEN+1];
	unsigned short port;
	char * path;
	unsigned int scope_id;
	char portstr[8];
	char * args_xml = NULL;
	struct evhttp_request * req;
	struct evkeyvalq * headers;
	struct evbuffer * buffer;

	if(arg_count > 0) {
		int i;
		size_t l, n;
		for(i = 0, l = 0; i < arg_count; i++) {
			/* <ELT>VAL</ELT> */
			l += strlen(args[i].elt) * 2 + strlen(args[i].val) + 5;
		}
		args_xml = malloc(++l);
		if(args_xml == NULL) {
			return -1;
		}
		for(i = 0, n = 0; i < arg_count && n < l; i++) {
			/* <ELT>VAL</ELT> */
			n += snprintf(args_xml + n, l - n, "<%s>%s</%s>",
			              args[i].elt, args[i].val, args[i].elt);
		}
	}

	body_len = snprintf(NULL, 0, fmt_soap, method, service, args_xml?args_xml:"", method);
	body = malloc(body_len + 1);
	if(body == NULL) {
		return -1;
	}
	if(snprintf(body, body_len + 1, fmt_soap, method, service, args_xml?args_xml:"", method) != body_len) {
		debug_printf("snprintf() returned strange value...\n");
	}
	free(args_xml);
	args_xml = NULL;
	if(!parseURL(url, hostname, &port, &path, &scope_id)) {
		return -1;
	}
	if(port != 80)
		snprintf(portstr, sizeof(portstr), ":%hu", port);
	else
		portstr[0] = '\0';
	snprintf(action, sizeof(action), "\"%s#%s\"", service, method);
	if(p->soap_conn == NULL) {
		p->soap_conn = evhttp_connection_base_new(p->parent->base, NULL, hostname, port);
	}
	req = evhttp_request_new(upnpc_soap_response, p);
	headers = evhttp_request_get_output_headers(req);
	buffer = evhttp_request_get_output_buffer(req);
	evhttp_add_header(headers, "Host", hostname);
	evhttp_add_header(headers, "SOAPAction", action);
	evhttp_add_header(headers, "Content-Type", "text/xml");
	/*evhttp_add_header(headers, "User-Agent", "***");*/
	/*evhttp_add_header(headers, "Cache-Control", "no-cache");*/
	/*evhttp_add_header(headers, "Pragma", "no-cache");*/
	evbuffer_add(buffer, body, body_len);
	evhttp_make_request(p->soap_conn, req, EVHTTP_REQ_POST, path);
	free(body);
	p->state |= UPNPC_DEVICE_SOAP_REQ;
	return 0;
}

/* public functions */
int upnpc_init(upnpc_t * p, struct event_base * base, const char * multicastif,
               upnpc_callback_fn ready_cb, upnpc_callback_fn soap_cb, void * cb_data)
{
	int opt = 1;
	struct sockaddr_in addr;
	struct timeval timeout;

	if(p == NULL || base == NULL)
		return UPNPC_ERR_INVALID_ARGS;
	memset(p, 0, sizeof(upnpc_t)); /* clean everything */
	p->base = base;
	p->ready_cb = ready_cb;
	p->soap_cb = soap_cb;
	p->cb_data = cb_data;
	/* open the socket for SSDP */
	p->ssdp_socket = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if(p->ssdp_socket < 0) {
		return UPNPC_ERR_SOCKET_FAILED;
	}
	/* set REUSEADDR */
#ifdef _WIN32
	if(setsockopt(p->ssdp_socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt)) < 0) {
#else /* _WIN32 */
	if(setsockopt(p->ssdp_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
#endif /* _WIN32 */
		/* non fatal error ! */
		debug_printf("setsockopt(%d, SOL_SOCKET, SO_REUSEADDR, ...) FAILED\n", p->ssdp_socket);
	}
	if(evutil_make_socket_nonblocking(p->ssdp_socket) < 0) {
		debug_printf("evutil_make_socket_nonblocking FAILED\n");
	}

	/* receive address */
	memset(&addr, 0, sizeof(struct sockaddr_in));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	/*addr.sin_port = htons(SSDP_PORT);*/

	if(multicastif) {
		struct in_addr mc_if;
		mc_if.s_addr = inet_addr(multicastif);
    	addr.sin_addr.s_addr = mc_if.s_addr;
		if(setsockopt(p->ssdp_socket, IPPROTO_IP, IP_MULTICAST_IF, (const char *)&mc_if, sizeof(mc_if)) < 0) {
			PRINT_SOCKET_ERROR("setsockopt");
			/* non fatal error ! */
		}
	}

	/* bind the socket to the ssdp address in order to receive responses */
	if(bind(p->ssdp_socket, (struct sockaddr *)&addr, sizeof(struct sockaddr_in)) != 0) {
		close(p->ssdp_socket);
		return UPNPC_ERR_BIND_FAILED;
	}
	/* event on SSDP */
	p->ev_ssdp_recv = event_new(p->base, p->ssdp_socket,
	                         EV_READ|EV_PERSIST,
	                         (event_callback_fn)upnpc_receive_and_parse_ssdp, p);
	timeout.tv_sec = 3;
	timeout.tv_usec = 0;
	if(event_add(p->ev_ssdp_recv, &timeout)) {
		debug_printf("event_add FAILED\n");
	}
	p->ev_ssdp_writable = event_new(p->base, p->ssdp_socket,
	                             EV_WRITE,
	                             (event_callback_fn)upnpc_send_ssdp_msearch, p);
	if(event_add(p->ev_ssdp_writable, NULL)) {
		debug_printf("event_add FAILED\n");
	}
	return UPNPC_OK;
}

static void upnpc_device_finalize(upnpc_device_t * d)
{
	d->state = 0;
	free(d->root_desc_location);
	d->root_desc_location = NULL;
	free(d->control_cif_url);
	d->control_cif_url = NULL;
	free(d->cif_service_type);
	d->cif_service_type = NULL;
	free(d->control_conn_url);
	d->control_conn_url = NULL;
	free(d->conn_service_type);
	d->conn_service_type = NULL;
	if(d->desc_conn) {
		evhttp_connection_free(d->desc_conn);
		d->desc_conn = NULL;
	}
	if(d->soap_conn) {
		evhttp_connection_free(d->soap_conn);
		d->soap_conn = NULL;
	}
	ClearNameValueList(&d->soap_response_data);
}

int upnpc_finalize(upnpc_t * p)
{
	if(!p) return UPNPC_ERR_INVALID_ARGS;
	p->discover_device_index = 0;
	if(p->ssdp_socket >= 0) {
		close(p->ssdp_socket);
		p->ssdp_socket = -1;
	}
	if(p->ev_ssdp_recv) {
		event_free(p->ev_ssdp_recv);
		p->ev_ssdp_recv = NULL;
	}
	if(p->ev_ssdp_writable) {
		event_free(p->ev_ssdp_writable);
		p->ev_ssdp_writable = NULL;
	}
	while(p->devices != NULL) {
		upnpc_device_t * d = p->devices;
		upnpc_device_finalize(d);
		p->devices = d->next;
		free(d);
	}
	return UPNPC_OK;
}

int upnpc_get_external_ip_address(upnpc_device_t * p)
{
	return upnpc_send_soap_request(p, p->control_conn_url,
	                         p->conn_service_type/*"urn:schemas-upnp-org:service:WANIPConnection:1"*/,
	                         "GetExternalIPAddress", NULL, 0);
}

int upnpc_get_link_layer_max_rate(upnpc_device_t * p)
{
	return upnpc_send_soap_request(p, p->control_cif_url,
	                         p->cif_service_type/*"urn:schemas-upnp-org:service:WANCommonInterfaceConfig:1"*/,
	                         "GetCommonLinkProperties", NULL, 0);
}

int upnpc_delete_port_mapping(upnpc_device_t * p,
                              const char * remote_host, unsigned short ext_port,
                              const char * proto)
{
	struct upnp_args args[3];
	char ext_port_str[8];

	if(proto == NULL || ext_port == 0)
		return UPNPC_ERR_INVALID_ARGS;
	snprintf(ext_port_str, sizeof(ext_port_str), "%hu", ext_port);
	args[0].elt = "NewRemoteHost";
	args[0].val = remote_host?remote_host:"";
	args[1].elt = "NewExternalPort";
	args[1].val = ext_port_str;
	args[2].elt = "NewProtocol";
	args[2].val = proto;
	return upnpc_send_soap_request(p, p->control_conn_url,
	                         p->conn_service_type,/*"urn:schemas-upnp-org:service:WANIPConnection:1",*/
	                         "DeletePortMapping",
	                         args, 3);
}

int upnpc_add_port_mapping(upnpc_device_t * p,
                           const char * remote_host, unsigned short ext_port,
                           unsigned short int_port, const char * int_client,
                           const char * proto, const char * description,
                           unsigned int lease_duration)
{
	struct upnp_args args[8];
	char lease_duration_str[16];
	char int_port_str[8];
	char ext_port_str[8];

	if(int_client == NULL || int_port == 0 || ext_port == 0 || proto == NULL)
		return UPNPC_ERR_INVALID_ARGS;
	snprintf(lease_duration_str, sizeof(lease_duration_str), "%u", lease_duration);
	snprintf(int_port_str, sizeof(int_port_str), "%hu", int_port);
	snprintf(ext_port_str, sizeof(ext_port_str), "%hu", ext_port);
	args[0].elt = "NewRemoteHost";
	args[0].val = remote_host?remote_host:"";
	args[1].elt = "NewExternalPort";
	args[1].val = ext_port_str;
	args[2].elt = "NewProtocol";
	args[2].val = proto;
	args[3].elt = "NewInternalPort";
	args[3].val = int_port_str;
	args[4].elt = "NewInternalClient";
	args[4].val = int_client;
	args[5].elt = "NewEnabled";
	args[5].val = "1";
	args[6].elt = "NewPortMappingDescription";
	args[6].val = description?description:"miniupnpc-libevent";
	args[7].elt = "NewLeaseDuration";
	args[7].val = lease_duration_str;
	return upnpc_send_soap_request(p, p->control_conn_url,
	                         p->conn_service_type/*"urn:schemas-upnp-org:service:WANIPConnection:1"*/,
	                         "AddPortMapping",
	                         args, 8);
}

int upnpc_get_status_info(upnpc_device_t * p)
{
	return upnpc_send_soap_request(p, p->control_conn_url,
	                         p->conn_service_type/*"urn:schemas-upnp-org:service:WANIPConnection:1"*/,
	                         "GetStatusInfo", NULL, 0);
}
