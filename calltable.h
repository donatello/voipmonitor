/* Martin Vit support@voipmonitor.org
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2.
*/

/* Calls are stored into indexed array. 
 * Into one calltable is stored SIP call-id and IP-port of SDP session
 */
#include <queue>

#include <arpa/inet.h>
#include <time.h>

#include <pcap.h>
#include <mysql++.h>

#include "rtp.h"

#define MAX_IP_PER_CALL 5	//!< total maxumum of SDP sessions for one call-id
#define MAX_SSRC_PER_CALL 10	//!< total maxumum of SDP sessions for one call-id
#define MAX_CALL_ID 32		//!< max len of stored call-id
#define MAX_FNAME 256		//!< max len of stored call-id
#define RTPTIMEOUT 300


/**
  * This class implements operations on call
*/
class Call {
public:
	unsigned long call_id_len;	//!< length of call-id 	
	char call_id[MAX_CALL_ID];	//!< call-id from SIP session
	char fbasename[MAX_FNAME];	//!< basename of file 
	char caller[256];		//!< From: xxx 
	char called[256];		//!< To: xxx
	char byecseq[32];		//!< To: xxx
	char invitecseq[32];		//!< To: xxx
	bool seeninvite;		//!< true if we see SIP INVITE within the Call
	bool seeninviteok;			//!< true if we see SIP INVITE within the Call
	bool seenbye;			//!< true if we see SIP BYE within the Call
	bool seenbyeandok;		//!< true if we see SIP OK TO BYE OR TO CANEL within the Call
	bool sighup;			//!< true if call is saving during sighup
	char *dirname();		//!< name of the directory to store files for the Call
	char ua[MAX_IP_PER_CALL][1024];	//!< user agent 
	RTP tmprtp;			//!< temporary structure used to decode information from frame
	
	/**
	 * constructor
	 *
	 * @param call_id unique identification of call parsed from packet
	 * @param call_id_len lenght of the call_id buffer
	 * @param time time of the first packet
	 * 
	*/
	Call(char *call_id, unsigned long call_id_len, time_t time);

	/**
	 * @brief find Call by IP adress and port. 
	 *
	 * This function is applied for every incoming UDP packet
	 *
	 * @param addr IP address of the packet
	 * @param port port number of the packet
	 * 
	 * @return reference to the finded Call or NULL if not found. 
	*/
	Call *find_by_ip_port(in_addr_t addr, unsigned short port);

	/**
	 * @brief read RTP packet 
	 *
	 * Used for reading RTP packet 
	 *
	 * @param data pointer to the packet buffer
	 * @param datalen lenght of the buffer
	 * @param header header structure of the packet
	 * @param saddr source IP adress of the packet
	 * 
	*/
	void read_rtp( unsigned char *data, unsigned long datalen, struct pcap_pkthdr *header,  u_int32_t saddr);

	/**
	 * @brief adds RTP stream to the this Call 
	 *
	 * Adds RTP stream to the this Call which is identified by IP address and port number
	 *
	 * @param addr IP address of the RTP stream
	 * @param port port number of the RTP stream
	 * 
	 * @return return 0 on success, 1 if IP and port is duplicated and -1 on failure
	*/
	int add_ip_port(in_addr_t addr, unsigned short port, char *ua, unsigned long ua_len);

	/**
	 * @brief get file descriptor of the writing pcap file  
	 *
	 * @return file descriptor of the writing pcap file
	*/
	pcap_dumper_t *get_f_pcap() { return f_pcap; };
	
	/**
	 * @brief set file descriptor of the writing pcap file  
	 *
	 * @param file descriptor
	*/
	void set_f_pcap(pcap_dumper_t *f_pcap) { this->f_pcap = f_pcap; };

	/**
	 * @brief get time of the last seen packet which belongs to this call
	 *
	 * @param f_pcap file descriptor 
	 *
	 * @return time of the last packet in seconds from UNIX epoch
	*/
	time_t get_last_packet_time() { return last_packet_time; };

	/**
	 * @brief set time of the last seen packet which belongs to this call
	 *
	 * this time is used for calculating lenght of the call
	 *
	 * @param timestamp in seconds from UNIX epoch
	 *
	*/
	void set_last_packet_time(time_t mtime) { last_packet_time = mtime; };

	/**
	 * @brief get first time of the the packet which belongs to this call
	 *
	 * this time is used as start of the call in CDR record
	 *
	 * @return time of the first packet in seconds from UNIX epoch
	*/
	time_t get_first_packet_time() { return first_packet_time; };

	/**
	 * @brief set first time of the the packet which belongs to this call
	 *
	 * @param timestamp in seconds from UNIX epoch
	 *
	*/
	void set_first_packet_time(time_t mtime) { first_packet_time = mtime; };

	/**
	 * @brief transfer Call to another queue which periodicaly stores Call to MySQL
	 *
	*/
	int saveToMysql();

	/**
	 * @brief calculate duration of the call
	 *
	 * @return lenght of the call in seconds
	*/
	unsigned int duration() { return last_packet_time - first_packet_time; };
	
	/**
	 * @brief return start of the call which is first seen packet 
	 *
	 * @param timestamp in seconds from UNIX epoch
	*/
	unsigned int calltime() { return first_packet_time; };


	/**
	 * @brief print debug information for the call to stdout
	 *
	*/
	void dump();

	
private:
	in_addr_t addr[MAX_IP_PER_CALL];	//!< IP address from SDP (indexed together with port)
	unsigned short port[MAX_IP_PER_CALL];	//!< port number from SDP (indexed together with IP)
	int ipport_n;				//!< last index of addr and port array 
	RTP rtp[MAX_SSRC_PER_CALL];		//!< array of RTP streams
	int ssrc_n;				//!< last index of rtp array
	time_t last_packet_time;	
	time_t first_packet_time;	
	pcap_dumper_t *f_pcap;
	char sdirname[255];
};

/**
  * This class implements operations on Call list
*/
class Calltable {
public:
	queue<Call*> calls_queue; //!< this queue is used for asynchronous storing CDR by the worker thread
	list<Call*> calls_list; //!< 
	list<Call*>::iterator call;
	
	/**
	 * @brief constructor
	 *
	*/
	Calltable() { pthread_mutex_init(&qlock, NULL); };

	/**
	 * @brief lock calls_queue structure 
	 *
	*/
	void lock_calls_queue() { pthread_mutex_lock(&qlock); };

	/**
	 * @brief unlock calls_queue structure 
	 *
	*/
	void unlock_calls_queue() { pthread_mutex_unlock(&qlock); };
	
	/**
	 * @brief add Call to Calltable
	 *
	 * @param call_id unique identifier of the Call which is parsed from the SIP packets
	 * @param call_id_len lenght of the call_id buffer
	 * @param time timestamp of arrivel packet in seconds from UNIX epoch
	 *
	 * @return reference of the new Call class
	*/
	Call *add(char *call_id, unsigned long call_id_len, time_t time);

	/**
	 * @brief find Call by call_id
	 *
	 * @param call_id unique identifier of the Call which is parsed from the SIP packets
	 * @param call_id_len lenght of the call_id buffer
	 *
	 * @return reference of the Call if found, otherwise return NULL
	*/
	Call *find_by_call_id(char *call_id, unsigned long call_id_len);

	/**
	 * @brief find Call by IP adress and port number
	 *
	 * @param addr IP address of the packet
	 * @param port port number of the packet
	 *
	 * @return reference of the Call if found, otherwise return NULL
	*/
	Call *find_by_ip_port(in_addr_t addr, unsigned short port);

	/**
	 * @brief Save inactive calls to MySQL and delete it from list
	 *
	 *
	 * walk this list of Calls and if any of the call is inactive more
	 * than 5 minutes, save it to MySQL and delete it from the list
	 *
	 * @param cuutime current time
	 *
	 * @return reference of the Call if found, otherwise return NULL
	*/
	int cleanup( time_t currtime );

private:
	pthread_mutex_t qlock;	//!< mutex locking calls_queue
	

};