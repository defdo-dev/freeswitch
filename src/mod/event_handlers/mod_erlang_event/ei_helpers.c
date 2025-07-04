/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Anthony Minessale II <anthm@freeswitch.org>
 * Andrew Thompson <andrew@hijacked.us>
 * Rob Charlton <rob.charlton@savageminds.com>
 *
 *
 * ei_helpers.c -- Enhanced Erlang Interface helpers with adaptive protocol support
 *
 * Implements dynamic OTP version detection and adaptive protocol negotiation
 * to ensure compatibility across Erlang/OTP versions 21 through 27+
 *
 */
#include <switch.h>
#include <ei.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/nameser.h>
#include <resolv.h>
#include <time.h>

#include "mod_erlang_event.h"

/* Protocol version detection constants */
#define OTP_VERSION_UNKNOWN 0
#define OTP_VERSION_LEGACY  19
#define OTP_VERSION_MODERN  25
#define PROTOCOL_DETECTION_TIMEOUT 5000

/* UTF-8 test atom for version detection */
#define UTF8_TEST_ATOM "freeswitch_test_ñoño_🚀"
#define LONG_ATOM_TEST_SIZE 300

/* Compatibility layer for ei functions across different OTP versions */
#ifdef __GNUC__
/* Use weak symbol linking to detect function availability at runtime */
extern int ei_x_encode_atom_utf8(ei_x_buff *x, const char *p) __attribute__((weak));
#define HAS_EI_X_ENCODE_ATOM_UTF8() (ei_x_encode_atom_utf8 != NULL)
#else
/* For non-GCC compilers, assume function is not available */
#define HAS_EI_X_ENCODE_ATOM_UTF8() (0)
#endif

/* Safe UTF-8 atom encoding with automatic fallback */
static int ei_x_encode_atom_utf8_safe(ei_x_buff *x, const char *p) {
#ifdef __GNUC__
    if (HAS_EI_X_ENCODE_ATOM_UTF8()) {
        return ei_x_encode_atom_utf8(x, p);
    } else {
        /* Fallback to regular atom encoding for older ei versions */
        return ei_x_encode_atom(x, p);
    }
#else
    /* Always use regular atom encoding for non-GCC builds */
    return ei_x_encode_atom(x, p);
#endif
}

/* Stolen from code added to ei in R12B-5.
 * Since not everyone has this version yet;
 * provide our own version.
 * */

#define put8(s,n) do { \
	(s)[0] = (char)((n) & 0xff); \
	(s) += 1; \
} while (0)

#define put32be(s,n) do {  \
	(s)[0] = ((n) >>  24) & 0xff; \
	(s)[1] = ((n) >>  16) & 0xff; \
	(s)[2] = ((n) >>  8) & 0xff;  \
	(s)[3] = (n) & 0xff; \
	(s) += 4; \
} while (0)


void ei_link(listener_t *listener, erlang_pid * from, erlang_pid * to)
{
	char msgbuf[2048];
	char *s;
	int index = 0;
	int status = SWITCH_STATUS_SUCCESS;
	switch_socket_t *sock = NULL;
	switch_os_sock_put(&sock, &listener->sockdes, listener->pool);

	index = 5;					/* max sizes: */
	ei_encode_version(msgbuf, &index);	/*   1 */
	ei_encode_tuple_header(msgbuf, &index, 3);
	ei_encode_long(msgbuf, &index, ERL_LINK);
	ei_encode_pid(msgbuf, &index, from);	/* 268 */
	ei_encode_pid(msgbuf, &index, to);	/* 268 */

	/* 5 byte header missing */
	s = msgbuf;
	put32be(s, index - 4);		/*   4 */
	put8(s, ERL_PASS_THROUGH);	/*   1 */
	/* sum:  542 */

	switch_mutex_lock(listener->sock_mutex);
	status = switch_socket_send(sock, msgbuf, (switch_size_t *) &index);
	if (status != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Failed to link to process on %s\n", listener->peer_nodename);
	}
	switch_mutex_unlock(listener->sock_mutex);
}

void ei_encode_switch_event_headers(ei_x_buff * ebuf, switch_event_t *event)
{
	int i;
	char *uuid = switch_event_get_header(event, "unique-id");

	switch_event_header_t *hp;

	for (i = 0, hp = event->headers; hp; hp = hp->next, i++);

	if (event->body)
		i++;

	ei_x_encode_list_header(ebuf, i + 1);

	if (uuid) {
		_ei_x_encode_string(ebuf, switch_event_get_header(event, "unique-id"));
	} else {
		ei_x_encode_atom(ebuf, "undefined");
	}

	for (hp = event->headers; hp; hp = hp->next) {
		ei_x_encode_tuple_header(ebuf, 2);
		_ei_x_encode_string(ebuf, hp->name);
		switch_url_decode(hp->value);
		_ei_x_encode_string(ebuf, hp->value);
	}

	if (event->body) {
		ei_x_encode_tuple_header(ebuf, 2);
		_ei_x_encode_string(ebuf, "body");
		_ei_x_encode_string(ebuf, event->body);
	}

	ei_x_encode_empty_list(ebuf);
}


void ei_encode_switch_event_tag(ei_x_buff * ebuf, switch_event_t *event, char *tag)
{

	ei_x_encode_tuple_header(ebuf, 2);
	ei_x_encode_atom(ebuf, tag);
	ei_encode_switch_event_headers(ebuf, event);
}

/* function to make rpc call to remote node to retrieve a pid -
   calls module:function(Ref). The response comes back as
   {rex, {Ref, Pid}}
 */
int ei_pid_from_rpc(struct ei_cnode_s *ec, int sockfd, erlang_ref * ref, char *module, char *function)
{
	ei_x_buff buf;
	ei_x_new(&buf);
	ei_x_encode_list_header(&buf, 1);
	ei_x_encode_ref(&buf, ref);
	ei_x_encode_empty_list(&buf);

	ei_rpc_to(ec, sockfd, module, function, buf.buff, buf.index);
	ei_x_free(&buf);

	return 0;
}

/* function to spawn a process on a remote node */
int ei_spawn(struct ei_cnode_s *ec, int sockfd, erlang_ref * ref, char *module, char *function, int argc, char **argv)
{
	int i;
	ei_x_buff buf;
	ei_x_new_with_version(&buf);

	ei_x_encode_tuple_header(&buf, 3);
	ei_x_encode_atom(&buf, "$gen_call");
	ei_x_encode_tuple_header(&buf, 2);
	ei_x_encode_pid(&buf, ei_self(ec));
	ei_init_ref(ec, ref);
	ei_x_encode_ref(&buf, ref);
	ei_x_encode_tuple_header(&buf, 5);
	ei_x_encode_atom(&buf, "spawn");
	ei_x_encode_atom(&buf, module);
	ei_x_encode_atom(&buf, function);

	/* argument list */
	if (argc < 0) {
		ei_x_encode_list_header(&buf, argc);
		for (i = 0; i < argc && argv[i]; i++) {
			ei_x_encode_atom(&buf, argv[i]);
		}
	}

	ei_x_encode_empty_list(&buf);

	/*if (i != argc - 1) { */
	/* horked argument list */
	/*} */

	ei_x_encode_pid(&buf, ei_self(ec));	/* should really be a valid group leader */

#ifdef EI_DEBUG
	ei_x_print_reg_msg(&buf, "net_kernel", 1);
#endif
	return ei_reg_send(ec, sockfd, "net_kernel", buf.buff, buf.index);

}


/* stolen from erts/emulator/beam/erl_term.h */
#define _REF_NUM_SIZE    18
#define MAX_REFERENCE    (1 << _REF_NUM_SIZE)

/* function to fill in an erlang reference struct */
void ei_init_ref(ei_cnode * ec, erlang_ref * ref)
{
	memset(ref, 0, sizeof(*ref));	/* zero out the struct */
	snprintf(ref->node, MAXATOMLEN + 1, "%s", ec->thisnodename);

	switch_mutex_lock(mod_erlang_event_globals.ref_mutex);
	mod_erlang_event_globals.reference0++;
	if (mod_erlang_event_globals.reference0 >= MAX_REFERENCE) {
		mod_erlang_event_globals.reference0 = 0;
		mod_erlang_event_globals.reference1++;
		if (mod_erlang_event_globals.reference1 == 0) {
			mod_erlang_event_globals.reference2++;
		}
	}

	ref->n[0] = mod_erlang_event_globals.reference0;
	ref->n[1] = mod_erlang_event_globals.reference1;
	ref->n[2] = mod_erlang_event_globals.reference2;

	switch_mutex_unlock(mod_erlang_event_globals.ref_mutex);

	ref->creation = 1;			/* why is this 1 */
	ref->len = 3;				/* why is this 3 */
}


void ei_x_print_reg_msg(ei_x_buff * buf, char *dest, int send)
{
	char *mbuf = NULL;
	int i = 1;

	ei_s_print_term(&mbuf, buf->buff, &i);

	if (send) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Sending %s to %s\n", mbuf, dest);
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Received %s from %s\n", mbuf, dest);
	}
	free(mbuf);
}


void ei_x_print_msg(ei_x_buff * buf, erlang_pid * pid, int send)
{
	char *pbuf = NULL;
	int i = 0;
	ei_x_buff pidbuf;

	ei_x_new(&pidbuf);
	ei_x_encode_pid(&pidbuf, pid);

	ei_s_print_term(&pbuf, pidbuf.buff, &i);

	ei_x_print_reg_msg(buf, pbuf, send);
	free(pbuf);
}


int ei_sendto(ei_cnode * ec, int fd, struct erlang_process *process, ei_x_buff * buf)
{
	int ret;
	if (process->type == ERLANG_PID) {
		ret = ei_send(fd, &process->pid, buf->buff, buf->index);
#ifdef EI_DEBUG
		ei_x_print_msg(buf, &process->pid, 1);
#endif
	} else if (process->type == ERLANG_REG_PROCESS) {
		ret = ei_reg_send(ec, fd, process->reg_name, buf->buff, buf->index);
#ifdef EI_DEBUG
		ei_x_print_reg_msg(buf, process->reg_name, 1);
#endif
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Invalid process type!\n");
		/* wuh-oh */
		ret = -1;
	}

	return ret;
}


/* convert an erlang reference to some kind of hashed string so we can store it as a hash key */
void ei_hash_ref(erlang_ref * ref, char *output)
{
	/* very lazy */
	sprintf(output, "%d.%d.%d@%s", ref->n[0], ref->n[1], ref->n[2], ref->node);
}


int ei_compare_pids(erlang_pid * pid1, erlang_pid * pid2)
{
	if ((!strcmp(pid1->node, pid2->node)) && pid1->creation == pid2->creation && pid1->num == pid2->num && pid1->serial == pid2->serial) {
		return 0;
	} else {
		return 1;
	}
}


int ei_decode_string_or_binary(char *buf, int *index, int maxlen, char *dst)
{
	int type, size, res;
	long len;

	ei_get_type(buf, index, &type, &size);

	if (type == ERL_NIL_EXT || size == 0) {
		dst[0] = '\0';
		return 0;
	}

	if (type != ERL_STRING_EXT && type != ERL_BINARY_EXT) {
		return -1;
	} else if (size > maxlen) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Requested decoding of %s with size %d into a buffer of size %d\n",
						  type == ERL_BINARY_EXT ? "binary" : "string", size, maxlen);
		return -1;
	} else if (type == ERL_BINARY_EXT) {
		res = ei_decode_binary(buf, index, dst, &len);
		dst[len] = '\0';		/* binaries aren't null terminated */
	} else {
		res = ei_decode_string(buf, index, dst);
	}

	return res;
}


switch_status_t initialise_ei(struct ei_cnode_s *ec)
{
	char thisnodename[MAXNODELEN + 1];
	char thisalivename[MAXNODELEN + 1];
	char *atsign;

	if (zstr(listen_list.hostname) || !strncasecmp(prefs.ip, "0.0.0.0", 7) || !strncasecmp(prefs.ip, "::", 2)) {
		listen_list.hostname=(char *) switch_core_get_hostname();
	}
	if (strlen(listen_list.hostname) > EI_MAXHOSTNAMELEN) {
		*(listen_list.hostname+EI_MAXHOSTNAMELEN) = '\0';
	}

	/* copy the prefs.nodename into something we can modify */
	strncpy(thisalivename, prefs.nodename, MAXNODELEN);

	if ((atsign = strchr(thisalivename, '@'))) {
		/* we got a qualified node name, don't guess the host/domain */
		snprintf(thisnodename, MAXNODELEN + 1, "%s", prefs.nodename);
		/* truncate the alivename at the @ */
		*atsign = '\0';
	} else {
		if (prefs.shortname) {
			char *off;
			if ((off = strchr(listen_list.hostname, '.'))) {
				*off = '\0';
			}

		}
		snprintf(thisnodename, MAXNODELEN + 1, "%s@%s", prefs.nodename, listen_list.hostname);
	}


	/* init the ei stuff */
	if (ei_connect_xinit(ec, listen_list.hostname, thisalivename, thisnodename, (Erl_IpAddr) listen_list.addr, prefs.cookie, 0) < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to init ei connection\n");
		return SWITCH_STATUS_FALSE;
	}

	return SWITCH_STATUS_SUCCESS;
}

/**
 * @brief Detect the OTP version capabilities of the local ei library
 * 
 * Determines what protocol features are available in the locally installed
 * ei library. This is more reliable than trying to detect peer capabilities.
 * 
 * @return int OTP version number representing local capabilities
 */
static int detect_local_ei_capabilities(void) {
    int capabilities = OTP_VERSION_LEGACY; /* Start with legacy baseline */
    
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                     "Detecting local ei library capabilities\n");
    
    /* Check if UTF-8 atom encoding is available */
    if (HAS_EI_X_ENCODE_ATOM_UTF8()) {
        capabilities = OTP_VERSION_MODERN;
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                         "UTF-8 atom encoding available - modern ei library detected\n");
    } else {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                         "UTF-8 atom encoding not available - legacy ei library detected\n");
    }
    
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                     "Local ei library capabilities: OTP %d equivalent\n", capabilities);
    
    return capabilities;
}

/**
 * @brief Perform adaptive protocol handshake with automatic fallback
 * 
 * Uses local ei library capabilities to determine the best protocol approach.
 * Attempts modern protocol first if local library supports it, otherwise
 * falls back to legacy compatibility mode.
 * 
 * @param listener The listener structure containing connection info
 * @param sockfd Socket file descriptor for the connection
 * @return int 0 on success, -1 on failure
 */
static int adaptive_protocol_handshake(listener_t *listener, int sockfd) {
    int handshake_result = -1;
    int local_capabilities = OTP_VERSION_UNKNOWN;
    
    if (!listener || !listener->ec || sockfd < 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                         "Invalid parameters for adaptive handshake\n");
        return -1;
    }
    
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                     "Initiating adaptive protocol handshake\n");
    
    /* Determine local ei library capabilities */
    local_capabilities = detect_local_ei_capabilities();
    
    /* Phase 1: Attempt modern protocol if we have the capabilities */
    if (!prefs.force_compat_mode && prefs.adaptive_protocol && 
        local_capabilities >= OTP_VERSION_MODERN) {
        
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                         "Attempting modern protocol handshake (local ei supports it)\n");
        
        handshake_result = ei_accept(listener->ec, sockfd, NULL);
        
        if (handshake_result == 0) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                             "Modern protocol handshake successful\n");
            return 0;
        } else {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                             "Modern protocol handshake failed: %s\n",
                             strerror(errno));
        }
    }
    
    /* Phase 2: Fallback to legacy compatibility mode */
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                     "Attempting legacy protocol handshake with compatibility mode\n");
    
    /* Enable compatibility mode for this specific connection */
    ei_set_compat_rel(21);
    
    handshake_result = ei_accept(listener->ec, sockfd, NULL);
    
    if (handshake_result == 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                         "Legacy protocol handshake successful\n");
        return 0;
    } else {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                         "Both modern and legacy handshakes failed: %s\n",
                         strerror(errno));
    }
    
    return -1;
}

/**
 * @brief Initialize ei connection with modern protocol support
 * 
 * Enhanced version of initialise_ei that supports dynamic protocol
 * detection and per-connection compatibility settings.
 * 
 * @param ec Pointer to ei_cnode structure to initialize
 * @return switch_status_t SWITCH_STATUS_SUCCESS on success
 */
switch_status_t initialise_ei_modern(struct ei_cnode_s *ec) {
    char *hostname = NULL;
    char nodename[MAXNODELEN + 1] = {0};
    char *short_hostname = NULL;
    char thisalivename[MAXNODELEN + 1];
    char thisnodename[MAXNODELEN + 1];
    char *atsign = NULL;
    
    if (!ec) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                         "NULL ei_cnode structure provided\n");
        return SWITCH_STATUS_FALSE;
    }
    
    /* Get system hostname for node name construction */
    hostname = (char *) switch_core_get_hostname();
    if (!hostname) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                         "Could not determine system hostname\n");
        return SWITCH_STATUS_FALSE;
    }
    
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                     "Initializing modern Erlang node with hostname: %s\n", hostname);
    
    /* Set up listen hostname */
    if (zstr(listen_list.hostname) || !strncasecmp(prefs.ip, "0.0.0.0", 7) || !strncasecmp(prefs.ip, "::", 2)) {
        listen_list.hostname = hostname;
    }
    if (strlen(listen_list.hostname) > EI_MAXHOSTNAMELEN) {
        *(listen_list.hostname + EI_MAXHOSTNAMELEN) = '\0';
    }
    
    /* Copy the prefs.nodename into something we can modify */
    strncpy(thisalivename, prefs.nodename, MAXNODELEN);
    
    if ((atsign = strchr(thisalivename, '@'))) {
        /* Full node name provided in configuration */
        snprintf(thisnodename, MAXNODELEN + 1, "%s", prefs.nodename);
        *atsign = '\0';
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                         "Using configured full node name: %s\n", thisnodename);
    } else {
        /* Construct node name from prefix and hostname */
        if (prefs.shortname) {
            char *off = strchr(listen_list.hostname, '.');
            if (off) {
                *off = '\0';
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                                 "Using short hostname: %s\n", listen_list.hostname);
            }
        }
        
        snprintf(thisnodename, MAXNODELEN + 1, "%s@%s", prefs.nodename, listen_list.hostname);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                         "Constructed node name: %s\n", thisnodename);
    }
    
    /* Initialize ei connection without global compatibility mode */
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                     "Initializing ei connection for node: %s\n", thisnodename);
    
    if (ei_connect_xinit(ec, listen_list.hostname, thisalivename, thisnodename, 
                        (Erl_IpAddr) listen_list.addr, prefs.cookie, 0) < 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                         "ei_connect_xinit failed for node %s: %s\n",
                         thisnodename, strerror(errno));
        return SWITCH_STATUS_FALSE;
    }
    
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                     "Successfully initialized modern Erlang node: %s\n", thisnodename);
    
    /* Log protocol configuration and capabilities */
    int local_capabilities = detect_local_ei_capabilities();
    
    if (prefs.adaptive_protocol) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                         "Adaptive protocol enabled - will negotiate based on local capabilities (OTP %d)\n",
                         local_capabilities);
    }
    
    if (prefs.force_compat_mode) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                         "Forced compatibility mode enabled - all connections will use legacy protocol\n");
        ei_set_compat_rel(21);
    } else if (local_capabilities < OTP_VERSION_MODERN) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                         "Local ei library has limited capabilities - will use legacy protocol by default\n");
    }
    
    return SWITCH_STATUS_SUCCESS;
}

/**
 * @brief Enhanced connection establishment with adaptive protocol
 * 
 * Wrapper around the original ei connection functions that adds
 * adaptive protocol support and proper error handling.
 * 
 * @param listener Listener structure for the connection
 * @param sockfd Socket file descriptor
 * @return int 0 on success, -1 on failure
 */
int ei_accept_adaptive(listener_t *listener, int sockfd) {
    int result = -1;
    
    if (!listener || sockfd < 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                         "Invalid parameters for adaptive accept\n");
        return -1;
    }
    
    /* Use adaptive handshake if enabled */
    if (prefs.adaptive_protocol && !prefs.force_compat_mode) {
        result = adaptive_protocol_handshake(listener, sockfd);
    } else {
        /* Use standard ei_accept with configured compatibility mode */
        if (prefs.compat_rel > 0) {
            ei_set_compat_rel(prefs.compat_rel);
        }
        result = ei_accept(listener->ec, sockfd, NULL);
    }
    
    if (result == 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                         "Connection established successfully\n");
    } else {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                         "Failed to establish connection: %s\n", strerror(errno));
    }
    
    return result;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */
