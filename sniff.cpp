/* Martin Vit support@voipmonitor.org
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2.
*/

/*
This unit reads and parse packets from network interface or file 
and insert them into Call class. 

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>
#include <signal.h>
#include <endian.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <net/ethernet.h>
#include <syslog.h>

#include <pcap.h>

#include "calltable.h"
#include "sniff.h"

using namespace std;

#define MAX(x,y) ((x) > (y) ? (x) : (y))
#define MIN(x,y) ((x) < (y) ? (x) : (y))

#define INVITE 1
#define BYE 2
#define CANCEL 3
#define RES2XX 4
#define RES3XX 5
#define RES4XX 6
#define RES5XX 7
#define RES6XX 8


Calltable *calltable;
extern int opt_packetbuffered;	  // Make .pcap files writing ‘‘packet-buffered’’
extern int verbosity;
extern int terminating;

/* save packet into file */
void save_packet(Call *call, struct pcap_pkthdr *header, const u_char *packet) {
	if (call->get_f_pcap() != NULL){
		call->set_last_packet_time(header->ts.tv_sec);
		pcap_dump((u_char *) call->get_f_pcap(), header, packet);
		if (opt_packetbuffered) 
			pcap_dump_flush(call->get_f_pcap());
	}
}

/* get SIP tag from memory pointed to *ptr length of len */
char * gettag(const void *ptr, unsigned long len, const char *tag, unsigned long *gettaglen){
	unsigned long register r, l, tl;
	char *rc;

	tl = strlen(tag);
	r = (unsigned long)memmem(ptr, len, tag, tl);
	if(r == 0){
		l = 0;
	} else {
		r += tl;
		l = (unsigned long)memmem((void *)r, len - (r - (unsigned long)ptr), "\r\n", 2);
		if (l > 0){
			l -= r;
		} else {
			l = 0;
		}
	}
	rc = (char*)r;
	if (rc) {
		while (rc[0] == ' '){
			rc++;
			l--;
		}
	}
	*gettaglen = l;
	return rc;
}

int get_sip_peername(char *data, int data_len, char *tag, char *peername, int peername_len){
	unsigned long r, r2, peername_tag_len;
	char *peername_tag = gettag(data, data_len, tag, &peername_tag_len);
	if ((r = (unsigned long)memmem(peername_tag, peername_tag_len, "sip:", 4)) == 0){
		goto fail_exit;
	}
	r += 4;
	if ((r2 = (unsigned long)memmem(peername_tag, peername_tag_len, "@", 1)) == 0){
		goto fail_exit;
	}
	if (r2 <= r){
		goto fail_exit;
	}
	memcpy(peername, (void*)r, r2 - r);
	memset(peername + (r2 - r), 0, 1);
	return 0;
fail_exit:
	strcpy(peername, "empty");
	return 1;
}

int get_sip_branch(char *data, int data_len, char *tag, char *branch, int branch_len){
	unsigned long r, r2, branch_tag_len;
	char *branch_tag = gettag(data, data_len, tag, &branch_tag_len);
	if ((r = (unsigned long)memmem(branch_tag, branch_tag_len, "branch=", 7)) == 0){
		goto fail_exit;
	}
	r += 7;
	if ((r2 = (unsigned long)memmem(branch_tag, branch_tag_len, ";", 1)) == 0){
		goto fail_exit;
	}
	if (r2 <= r){
		goto fail_exit;
	}
	memcpy(branch, (void*)r, r2 - r);
	memset(branch + (r2 - r), 0, 1);
	return 0;
fail_exit:
	strcpy(branch, "");
	return 1;
}


int get_ip_port_from_sdp(char *sdp_text, in_addr_t *addr, unsigned short *port){
	unsigned long l;
	char *s;
	char s1[20];
	s=gettag(sdp_text,strlen(sdp_text), "c=IN IP4 ", &l);
	memset(s1, '\0', sizeof(s1));
	memcpy(s1, s, MIN(l, 19));
	if ((long)(*addr = inet_addr(s1)) == -1){
		*addr = 0;
		*port = 0;
		return 1;
	}
	s=gettag(sdp_text, strlen(sdp_text), "m=audio ", &l);
	if (l == 0 || (*port = atoi(s)) == 0){
		*port = 0;
		return 1;
	}
	return 0;
}

void readdump(pcap_t *handle) {
	struct pcap_pkthdr header;	// The header that pcap gives us
	const u_char *packet;		// The actual packet 
	unsigned long last_cleanup = 0;	// Last cleaning time
	struct iphdr *header_ip;
	struct udphdr *header_udp;
	char *data;
	unsigned long datalen;
	char *s;
	unsigned long l;
	char str1[1024],str2[1024];
	int sip_method = 0;
	Call *call;

	while (!terminating && (packet = pcap_next(handle, &header))){

		// checking and cleaning calltable every 15 seconds (if some packet arrive) 
		if (header.ts.tv_sec - last_cleanup > 15){
			if (last_cleanup >= 0){
				calltable->cleanup(header.ts.tv_sec);
			}
			last_cleanup = header.ts.tv_sec;
		}
	
		header_ip = (struct iphdr *) ((char*)packet + sizeof(struct ether_header));

		if (header_ip->protocol != IPPROTO_UDP) {
			//packet is not UDP, we are not interested, go to the next packet
			continue;
		}

		// prepare packet pointers 
		header_udp = (struct udphdr *) ((char *) header_ip + sizeof(*header_ip));
		data = (char *) header_udp + sizeof(*header_udp);
		datalen = header.len - ((unsigned long) data - (unsigned long) packet); 
		if ((call = calltable->find_by_ip_port(header_ip->daddr, htons(header_udp->dest)))){	
			// packet (RTP) by destination:port is already part of some stored call 
			call->read_rtp((unsigned char*) data, datalen, &header, header_ip->saddr);
			call->set_last_packet_time(header.ts.tv_sec);
			save_packet(call, &header, packet);
		} else if ((call = calltable->find_by_ip_port(header_ip->saddr, htons(header_udp->source)))){	
			call->read_rtp((unsigned char*) data, datalen, &header, header_ip->saddr);
			call->set_last_packet_time(header.ts.tv_sec);
			save_packet(call, &header, packet);
		} else if (htons(header_udp->source) == 5060 || htons(header_udp->dest) == 5060) {
			// packet is from or to port 5060 
			data[datalen]=0;
			/* No, this isn't the phone number of the caller. It uniquely represents 
			   the whole call, or dialog, between the two user agents. All related SIP 
			   messages use the same Call-ID. For example, when a user agent receives a 
			   BYE message, it knows which call to hang up based on the Call-ID.
			*/
			if(!(s = gettag(data,datalen,"Call-ID:", &l))) {
				continue;
			} else {
				if(l > 1023) {
					//XXX: funkce gettag nam vraci nekdy hodne dlouhe l, coz je bug, opravit az bude cas!
					continue;
				}
				memcpy(str1,s,l);
				str1[l] = '\0';
			}

			// parse SIP method 
			if ((datalen > 5) && !(memmem(data, 6, "INVITE", 6) == 0)) {
				if(verbosity > 2) 
					 syslog(LOG_NOTICE,"SIP msg: INVITE\n");
				sip_method = INVITE;
			} else if ((datalen > 2) && !(memmem(data, 3, "BYE", 3) == 0)) {
				if(verbosity > 2) 
					 syslog(LOG_NOTICE,"SIP msg: BYE\n");
				sip_method = BYE;
			} else if ((datalen > 5) && !(memmem(data, 6, "CANCEL", 6) == 0)) {
				if(verbosity > 2) 
					 syslog(LOG_NOTICE,"SIP msg: CANCEL\n");
				sip_method = CANCEL;
			} else if ((datalen > 8) && !(memmem(data, 9, "SIP/2.0 2", 9) == 0)) {
				if(verbosity > 2) 
					 syslog(LOG_NOTICE,"SIP msg: 2XX\n");
				sip_method = RES2XX;
			/*
			} else if ((datalen > 8) && !(memmem(data, 9, "SIP/2.0 3", 9) == 0)) {
				if(verbosity > 2) 
					 syslog(LOG_NOTICE,"SIP msg: 3XX\n");
				sip_method = RES3XX;
			} else if ((datalen > 8) && !(memmem(data, 9, "SIP/2.0 4", 9) == 0)) {
				if(verbosity > 2) 
					 syslog(LOG_NOTICE,"SIP msg: 4XX\n");
				sip_method = RES4XX;
			} else if ((datalen > 8) && !(memmem(data, 9, "SIP/2.0 5", 9) == 0)) {
				if(verbosity > 2) 
					 syslog(LOG_NOTICE,"SIP msg: 5XX\n");
				sip_method = RES5XX;
			} else if ((datalen > 8) && !(memmem(data, 9, "SIP/2.0 6", 9) == 0)) {
				if(verbosity > 2) 
					 syslog(LOG_NOTICE,"SIP msg: 6XX\n");
				sip_method = RES6XX;
			*/
			} else {
				if(verbosity > 2) {
					char a[100];
					strncpy(a, data, 20);
					a[20] = '\0';
					 syslog(LOG_NOTICE,"SIP msg: 1XX or Unknown msg %s\n", a);
				}
				sip_method = 0;
			}

			// find call */
			if (!(call = calltable->find_by_call_id(s, l))){
				// packet does not belongs to some call yet
				if (sip_method == INVITE) {
					// store this call only if it starts with invite
					call = calltable->add(s, l, header.ts.tv_sec);
					call->set_first_packet_time(header.ts.tv_sec);
					mkdir(call->dirname(), 0777);
					strcpy(call->fbasename, str1);
					sprintf(str2, "%s/%s.pcap", call->dirname(), str1);
					call->set_f_pcap(pcap_dump_open(handle, str2));

					//check and save CSeq for later to compare with OK 
					s = gettag(data, datalen, "CSeq:", &l);
					if(l) {
						memcpy(call->invitecseq, s, l);
						call->invitecseq[l] = '\0';
						if(verbosity > 2)
							syslog(LOG_NOTICE, "Seen invite, CSeq: %s\n", call->invitecseq);
					}
				} else {
					// SIP packet does not belong to any call and it is not INVITE 
					continue;
				}
			} else {
				// packet is already part of call
				call->set_last_packet_time(header.ts.tv_sec);
				// check if it is BYE or OK(RES2XX)
				if(sip_method == INVITE) {
					//check and save CSeq for later to compare with OK 
					s = gettag(data, datalen, "CSeq:", &l);
					if(l) {
						memcpy(call->invitecseq, s, l);
						call->invitecseq[l] = '\0';
						if(verbosity > 2)
							syslog(LOG_NOTICE, "Seen invite, CSeq: %s\n", call->invitecseq);
					}
				} else if(sip_method == BYE || sip_method == CANCEL) {
					//check and save CSeq for later to compare with OK 
					if((s = gettag(data, datalen, "CSeq:", &l))) {
						memcpy(call->byecseq, s, l);
						call->byecseq[l] = '\0';
						call->seenbye = true;
						if(verbosity > 2)
							syslog(LOG_NOTICE, "Seen bye\n");
							
					}
				} else if(sip_method == RES2XX) {

					// if it is OK check for BYE
					if((s = gettag(data, datalen, "CSeq:", &l))) {
						if(verbosity > 2)
							syslog(LOG_NOTICE, "Cseq: %s\n", data);
						if(strncmp(s, call->byecseq, l) == 0) {
							call->seenbyeandok = true;
							if(verbosity > 2)
								syslog(LOG_NOTICE, "Call closed\n");
						} else if(strncmp(s, call->invitecseq, l) == 0) {
							call->seeninviteok = true;
							if(verbosity > 2)
								syslog(LOG_NOTICE, "Call answered\n");
						}
					}
				}
				/*
				} else if(sip_method == RES3XX || sip_method == RES4XX || sip_method == RES5XX || sip_method == RES6XX) {
						call->seenbye = true;
						call->seenbyeandok = true;
						if(verbosity > 2)
							syslog(LOG_NOTICE, "Call closed2\n");
				}
				*/
			}
			
			/* this logic updates call on last INVITES */
			if (sip_method == INVITE) {
				get_sip_peername(data,datalen,"From:", call->caller, sizeof(call->caller));
				get_sip_peername(data,datalen,"To:", call->called, sizeof(call->called));
				call->seeninvite = true;
			}
			// SDP examination
			s = gettag(data,datalen,"Content-Type:",&l);
			if(l > 0 && strncasecmp(s, "application/sdp", l) == 0 && strstr(data, "\r\n\r\n") != NULL){
				// we have found SDP, add IP and port to the table
				in_addr_t tmp_addr;
				unsigned short tmp_port;
				if (!get_ip_port_from_sdp(strstr(data, "\r\n\r\n") + 1, &tmp_addr, &tmp_port)){
					// prepare User-Agent
					s = gettag(data,datalen,"User-Agent:", &l);
					// store RTP stream
					call->add_ip_port(tmp_addr, tmp_port, s, l);
	
				} else {
					if(verbosity >= 2){
						syslog(LOG_ERR, "Can't get ip/port from SDP:\n%s\n\n",strstr(data,"\r\n\r\n") + 1);
					}
				}
			}

			save_packet(call, &header, packet);
		} else {
			// we are not interested in this packet
			if (verbosity >= 6){
				char st1[16];
				char st2[16];
				struct in_addr in;

				in.s_addr = header_ip->saddr;
				strcpy(st1, inet_ntoa(in));
				in.s_addr = header_ip->daddr;
				strcpy(st2, inet_ntoa(in));
				syslog(LOG_ERR, "Skipping udp packet %s:%d->%s:%d\n",
							st1, htons(header_udp->source), st2, htons(header_udp->dest));
			}

		}
	}
}