/**
 * 
 * tcpdemux.cpp
 * A tcpip demultiplier.
 *
 * This file is part of tcpflow by Simson Garfinkel,
 * originally by Jeremy Elson <jelson@circlemud.org>
 *
 * This source code is under the GNU Public License (GPL).  See
 * LICENSE for details.
 *
 */

#include "tcpflow.h"

#include <iostream>
#include <sstream>
#include <vector>



tcpdemux::tcpdemux():outdir("."),flow_counter(0),packet_counter(0),
		     xreport(0),max_fds(10),flow_map(),start_new_connections(false),
		     openflows(),
		     opt(),fs()
		     
{
    /* Find out how many files we can have open safely...subtract 4 for
     * stdin, stdout, stderr, and the packet filter; one for breathing
     * room (we open new files before closing old ones), and one more to
     * be safe.
     */
    max_fds = get_max_fds() - NUM_RESERVED_FDS;
}

tcpdemux *tcpdemux::getInstance()
{
    static tcpdemux * theInstance = 0;
    if(theInstance==0) theInstance = new tcpdemux();
    return theInstance;
}




/**
 * Implement a list of openflows, each with an associated file descriptor.
 * When a new file needs to be opened, we can close a flow if necessary.
 */
void tcpdemux::close_all()
{
    for(tcpset::iterator it = openflows.begin();it!=openflows.end();it++){
	(*it)->close_file();
    }
    openflows.clear();
}


void tcpdemux::close_tcpip(tcpip *i)
{
    i->close_file();
    openflows.erase(i);
}

/**
 * find the flow that has been written to in the furthest past and close it.
 */
void tcpdemux::close_oldest()
{
    tcpip *oldest_tcp=0;
    for(tcpset::iterator it = openflows.begin();it!=openflows.end();it++){
	if(oldest_tcp==0 || (*it)->last_packet_number < oldest_tcp->last_packet_number){
	    oldest_tcp = (*it);
	}
    }
    if(oldest_tcp) close_tcpip(oldest_tcp);
}

/* Open a file, closing one of the existing flows f necessary.
 */
int tcpdemux::retrying_open(const std::string &filename,int oflag,int mask)
{
    while(true){
	if(openflows.size() >= max_fds) close_oldest();
	int fd = ::open(filename.c_str(),oflag,mask);
	DEBUG(2)("::open(%s,%d,%d)=%d",filename.c_str(),oflag,mask,fd);
	if(fd>=0) return fd;
	DEBUG(2)("retrying_open ::open failed with errno=%d",errno);
	if (errno != ENFILE && errno != EMFILE){
	    DEBUG(2)("retrying_open ::open failed with errno=%d (%s)",errno,strerror(errno));
	    return -1;		// wonder what it was
	}
	DEBUG(5) ("too many open files -- contracting FD ring to %d", max_fds);
	close_oldest();
    }
}

/* Find previously a previously created flow state in the database.
 */
tcpip *tcpdemux::find_tcpip(const flow_addr &flow)
{
    flow_map_t::const_iterator it = flow_map.find(flow);
    if (it==flow_map.end()){
	return NULL; // flow not found
    }
    return it->second;
}

/* Create a new flow state structure for a given flow.
 * Puts the flow in the map.
 * Returns a pointer to the new state.
 *
 * This is called by process_tcp
 *
 * NOTE: We keep pointers to tcp structures in the map, rather than
 * the structures themselves. This makes the map slightly more efficient,
 * since it doesn't need to shuffle entire structures.
 */

tcpip *tcpdemux::create_tcpip(const flow_addr &flowa, int32_t vlan,tcp_seq isn,
			      const timeval &ts,uint64_t connection_count)
{
    /* create space for the new state */
    flow flow(flowa,vlan,ts,ts,flow_counter++,connection_count);

    tcpip *new_tcpip = new tcpip(*this,flow,isn);
    new_tcpip->last_packet_number = packet_counter++;
    new_tcpip->nsn   = isn+1;		// expected
    DEBUG(5) ("%s: new flow. nsn:%d", new_tcpip->flow_pathname.c_str(),new_tcpip->nsn);
    flow_map[flow] = new_tcpip;
    return new_tcpip;
}

void tcpdemux::remove_flow(const flow_addr &flow)
{
    flow_map_t::iterator it = flow_map.find(flow);
    if(it!=flow_map.end()){
	close_tcpip(it->second);
	delete it->second;
	flow_map.erase(it);
    }
}

void tcpdemux::flow_map_clear()
{
    for(flow_map_t::iterator it=flow_map.begin();it!=flow_map.end();it++){
	delete it->second;
    }
    flow_map.clear();
}

int tcpdemux::open_tcpfile(tcpip *tcp)
{
    /* This shouldn't be called if the file is already open */
    assert(tcp->fd < 0);

    /* Now try and open the file */
    if(tcp->file_created) {
	DEBUG(5) ("%s: re-opening output file", tcp->flow_pathname.c_str());
	tcp->fd = retrying_open(tcp->flow_pathname,O_RDWR | O_BINARY | O_CREAT,0666);
    } else {
	DEBUG(5) ("%s: opening new output file", tcp->flow_pathname.c_str());
	tcp->fd = retrying_open(tcp->flow_pathname,O_RDWR | O_BINARY | O_CREAT,0666);
	tcp->file_created = true;		// remember we made it
    }

    /* If the file isn't open at this point, there's a problem */
    if (tcp->fd < 0 ) {
	/* we had some problem opening the file -- set FINISHED so we
	 * don't keep trying over and over again to reopen it
	 */
	perror(tcp->flow_pathname.c_str());
	return -1;
    }

    openflows.insert(tcp);
    tcp->pos = lseek(tcp->fd,(off_t)0,SEEK_END);	// seek to end
    tcp->nsn = tcp->isn + 1 + tcp->pos;			// byte 0 is seq=isn+1; note this will handle files > 4GiB
    return 0;
}


/****************************************************************
 *** tcpdemultiplexer 
 ****************************************************************/

/* Try to find the maximum number of FDs this system can have open */
unsigned int tcpdemux::get_max_fds(void)
{
    int max_descs = 0;
    const char *method;

    /* Use OPEN_MAX if it is available */
#if defined (OPEN_MAX)
    method = "OPEN_MAX";
    max_descs = OPEN_MAX;
#elif defined(RLIMIT_NOFILE)
    {
	struct rlimit limit;
	memset(&limit,0,sizeof(limit));

	method = "rlimit";
	if (getrlimit(RLIMIT_NOFILE, &limit) < 0) {
	    perror("getrlimit");
	    exit(1);
	}

	/* set the current to the maximum or specified value */
	if (max_desired_fds) limit.rlim_cur = max_desired_fds;
	else limit.rlim_cur = limit.rlim_max;

	if (setrlimit(RLIMIT_NOFILE, &limit) < 0) {
	    perror("setrlimit");
	    exit(1);
	}
	max_descs = limit.rlim_max;

#ifdef RLIM_INFINITY
	if (limit.rlim_max == RLIM_INFINITY) max_descs = MAX_FD_GUESS * 4;	/* pick a more reasonable max */
#endif
    }
#elif defined (_SC_OPEN_MAX)
    /* Okay, you don't have getrlimit() and you don't have OPEN_MAX.
     * Time to try the POSIX sysconf() function.  (See Stevens'
     * _Advanced Programming in the UNIX Environment_).  */
    method = "POSIX sysconf";
    errno = 0;
    if ((max_descs = sysconf(_SC_OPEN_MAX)) < 0) {
	if (errno == 0)
	    max_descs = MAX_FD_GUESS * 4;
	else {
	    perror("calling sysconf");
	    exit(1);
	}
    }

    /* if everything has failed, we'll just take a guess */
#else
    method = "random guess";
    max_descs = MAX_FD_GUESS;
#endif

    /* this must go here, after rlimit code */
    if (opt.max_desired_fds) {
	DEBUG(10) ("using only %d FDs", opt.max_desired_fds);
	return opt.max_desired_fds;
    }

    DEBUG(10) ("found max FDs to be %d using %s", max_descs, method);
    return max_descs;
}


/* write a portion of memory to the disk and a child fileobject. */
void tcpdemux::write_to_file(std::stringstream &xmlattr,
			     const std::string &fname,
			     const sbuf_t &sbuf)
{
    int fd = retrying_open(fname,O_WRONLY|O_CREAT|O_BINARY|O_TRUNC,0644);
    if(fd>=0){
	size_t count = sbuf.write(fd,0,sbuf.size());
	if(close(fd)!=0 || count!=sbuf.size()){
	    xmlattr << "<write_error errno='" << errno << "' buflen='" << sbuf.size() << "' count='" << count << "'>";
	} else {
	    xmlattr << "<byte_run file_offset='" << sbuf.pos0.offset << "' len='" << sbuf.size() << "'>"
		    << "<filename>" << fname << "</filename>"
		    << "<hashdigest type='MD5'>" << sbuf.md5().hexdigest() << "</hashdigest>"
		    << "</byte_run>\n";
	}
    }
}

/*
 * Called to processes a tcp packet
 */

#pragma GCC diagnostic ignored "-Wcast-align"
void tcpdemux::process_tcp(const struct timeval &ts,const u_char *data, uint32_t length,
			const ipaddr &src, const ipaddr &dst,int32_t vlan,sa_family_t family)
{
    if (length < sizeof(struct tcphdr)) {
	DEBUG(6) ("received truncated TCP segment!");
	return;
    }

    struct tcphdr *tcp_header = (struct tcphdr *) data;

    /* calculate the total length of the TCP header including options */
    u_int tcp_header_len = tcp_header->th_off * 4;

    /* fill in the flow_addr structure with info that identifies this flow */
    flow_addr this_flow(src,dst,ntohs(tcp_header->th_sport),ntohs(tcp_header->th_dport),family);

    tcp_seq seq  = ntohl(tcp_header->th_seq);
    bool syn_set = IS_SET(tcp_header->th_flags, TH_SYN);
    bool ack_set = IS_SET(tcp_header->th_flags, TH_ACK);

    //std::cerr << "\n*** process_tcp seq=" << seq << " \n";

    /* recalculate the beginning of data and its length, moving past the
     * TCP header
     */
    data   += tcp_header_len;
    length -= tcp_header_len;

    /* see if we have state about this flow; if not, create it */
    uint64_t connection_count = 0;
    int32_t  delta = 0;			// from current position in tcp connection; must be SIGNED 32 bit!
    tcpip   *tcp = find_tcpip(this_flow);
    
    /* If this_flow is not in the database and the start_new_connections flag is false, just return */
    if(tcp==0 && start_new_connections==false) return; 

    /* flow is in the database; find it */
    if(tcp){
	/* Compute delta based on next expected sequence number.
	 * If delta will be too much, start a new flow.
	 */
	delta = seq - tcp->nsn;		// notice that signed offset is calculated

	if(abs(delta) > opt.max_seek){
	    connection_count = tcp->myflow.connection_count+1;
	    remove_flow(this_flow);
	    tcp = 0;
	}
    }

    /* At this point, tcp may be NULL because:
     * case 1 - a connection wasn't found or because
     * case 2 - a new connections should be started (jump is too much)
     *
     * THIS IS THE ONLY PLACE THAT create_tcpip() is called.
     */

    if (tcp==NULL){
	/* Create a new connection.
	 * delta will be 0, because it's a new connection!
	 */
	tcp_seq isn = syn_set ? seq : seq-1;
	tcp = create_tcpip(this_flow, vlan, isn, ts,connection_count);
    }

    /* Now tcp is valid */
    tcp->myflow.tlast = ts;		// most recently seen packet
    tcp->myflow.packet_count++;

    /*
     * 2012-10-24 slg - the first byte is sent at SEQ==ISN+1.
     * The first byte in POSIX files have an LSEEK of 0.
     * The original code overcame this issue by introducing an intentional off-by-one
     * error with the statement tcp->isn++.
     * 
     * With the new TCP state-machine we simply follow the spec.
     *
     * The new state machine works by examining the SYN and ACK packets
     * in accordance with the TCP spec.
     */
    if(syn_set){
	if(tcp->syn_count>1){
	    DEBUG(1)("Multiple SYNs (%d) seen on a single connection.",tcp->syn_count);
	}
	tcp->syn_count++;
	if( ack_set ){
	    DEBUG(50) ("packet is handshake SYN"); /* First packet of three-way handshake */
	    tcp->dir = tcpip::dir_cs;	// client->server
	} else {
	    DEBUG(50) ("packet is handshake SYN/ACK"); /* second packet of three-way handshake  */
	    tcp->dir = tcpip::dir_sc;	// server->client
	}
	if(length>0){
	    tcp->violations++;
	    DEBUG(1) ("TCP PROTOCOL VIOLATION: SYN with data! (length=%d)",length);
	}
    }
    if(length==0) DEBUG(50) ("got TCP segment with no data"); // seems pointless to notify

    /* process any data.
     * Notice that this typically won't be called for the SYN or SYN/ACK,
     * since they both have no data by definition.
     */
    if (length>0){
	if (opt.console_output) {
	    tcp->print_packet(data, length);
	} else {
	    if (opt.opt_output_enabled){
		tcp->store_packet(data, length, delta);
	    }
	}
    }

    /* Finally, if there is a FIN, then kill this TCP connection*/
    if (IS_SET(tcp_header->th_flags, TH_FIN)){
	if(opt.opt_no_purge==false){
	    DEBUG(50)("packet is FIN; closing connection");
	    remove_flow(this_flow);	// take it out of the map
	}
    }
}
#pragma GCC diagnostic warning "-Wcast-align"


/* This is called when we receive an IPv4 datagram.  We make sure that
 * it's valid and contains a TCP segment; if so, we pass it to
 * process_tcp() for further processing.
 *
 * Note: we currently don't know how to handle IP fragments. */
#pragma GCC diagnostic ignored "-Wcast-align"
void tcpdemux::process_ip4(const struct timeval &ts,const u_char *data, uint32_t caplen,int32_t vlan)
{
    const struct ip *ip_header = (struct ip *) data;
    u_int ip_header_len;
    u_int ip_total_len;

    /* make sure that the packet is at least as long as the min IP header */
    if (caplen < sizeof(struct ip)) {
	DEBUG(6) ("received truncated IP datagram!");
	return;
    }

    DEBUG(100)("process_ip4. caplen=%d vlan=%d  ip_p=%d",(int)caplen,(int)vlan,(int)ip_header->ip_p);

    /* for now we're only looking for TCP; throw away everything else */
    if (ip_header->ip_p != IPPROTO_TCP) {
	DEBUG(50) ("got non-TCP frame -- IP proto %d", ip_header->ip_p);
	return;
    }

    /* check and see if we got everything.  NOTE: we must use
     * ip_total_len after this, because we may have captured bytes
     * beyond the end of the packet (e.g. ethernet padding).
     */
    ip_total_len = ntohs(ip_header->ip_len);
    if (caplen < ip_total_len) {
	DEBUG(6) ("warning: captured only %ld bytes of %ld-byte IP datagram",
		  (long) caplen, (long) ip_total_len);
    }

    /* XXX - throw away everything but fragment 0; this version doesn't
     * know how to do fragment reassembly.
     */
    if (ntohs(ip_header->ip_off) & 0x1fff) {
	DEBUG(2) ("warning: throwing away IP fragment from X to X");
	return;
    }

    /* figure out where the IP header ends */
    ip_header_len = ip_header->ip_hl * 4;

    /* make sure there's some data */
    if (ip_header_len > ip_total_len) {
	DEBUG(6) ("received truncated IP datagram!");
	return;
    }

    /* do TCP processing, faking an ipv6 address  */
    process_tcp(ts,data + ip_header_len, ip_total_len - ip_header_len,
		ipaddr(ip_header->ip_src.s_addr),
		ipaddr(ip_header->ip_dst.s_addr),
		vlan,AF_INET);
}
#pragma GCC diagnostic warning "-Wcast-align"


/* This is called when we receive an IPv6 datagram.
 *
 * Note: we don't support IPv6 extended headers
 */

struct private_ip6_hdr {
	union {
		struct ip6_hdrctl {
			uint32_t ip6_un1_flow;	/* 20 bits of flow-ID */
			uint16_t ip6_un1_plen;	/* payload length */
			uint8_t  ip6_un1_nxt;	/* next header */
			uint8_t  ip6_un1_hlim;	/* hop limit */
		} ip6_un1;
		uint8_t ip6_un2_vfc;	/* 4 bits version, top 4 bits class */
	} ip6_ctlun;
	struct private_in6_addr ip6_src;	/* source address */
	struct private_in6_addr ip6_dst;	/* destination address */
} __attribute__((__packed__));

/* These might be defined from an include file, so undef them to be sure */
#undef ip6_vfc
#undef ip6_flow
#undef ip6_plen	
#undef ip6_nxt	
#undef ip6_hlim	
#undef ip6_hops	

#define ip6_vfc		ip6_ctlun.ip6_un2_vfc
#define ip6_flow	ip6_ctlun.ip6_un1.ip6_un1_flow
#define ip6_plen	ip6_ctlun.ip6_un1.ip6_un1_plen
#define ip6_nxt		ip6_ctlun.ip6_un1.ip6_un1_nxt
#define ip6_hlim	ip6_ctlun.ip6_un1.ip6_un1_hlim
#define ip6_hops	ip6_ctlun.ip6_un1.ip6_un1_hlim

void tcpdemux::process_ip6(const struct timeval &ts,const u_char *data, const uint32_t caplen, const int32_t vlan)
{
    const struct private_ip6_hdr *ip_header = (struct private_ip6_hdr *) data;
    u_int16_t ip_payload_len;

    /* make sure that the packet is at least as long as the IPv6 header */
    if (caplen < sizeof(struct private_ip6_hdr)) {
	DEBUG(6) ("received truncated IPv6 datagram!");
	return;
    }


    /* for now we're only looking for TCP; throw away everything else */
    if (ip_header->ip6_nxt != IPPROTO_TCP) {
	DEBUG(50) ("got non-TCP frame -- IP proto %d", ip_header->ip6_nxt);
	return;
    }

    ip_payload_len = ntohs(ip_header->ip6_plen);

    /* make sure there's some data */
    if (ip_payload_len == 0) {
	DEBUG(6) ("received truncated IP datagram!");
	return;
    }

    /* do TCP processing */

    process_tcp(ts,
		data + sizeof(struct private_ip6_hdr),
		ip_payload_len,
		ipaddr(ip_header->ip6_src.s6_addr),
		ipaddr(ip_header->ip6_dst.s6_addr),
		vlan,AF_INET6);
}



/* This is called when we receive an IPv4 or IPv6 datagram.
 * This function calls process_ip4 or process_ip6
 */

#pragma GCC diagnostic ignored "-Wcast-align"
void tcpdemux::process_ip(const struct timeval &ts,const u_char *data, uint32_t caplen,int32_t vlan)
{
    const struct ip *ip_header = (struct ip *) data;
    if (caplen < sizeof(struct ip)) {
	DEBUG(6) ("can't determine IP datagram version!");
	return;
    }

    if(ip_header->ip_v == 6) {
	process_ip6(ts,data, caplen,vlan);
    } else {
	process_ip4(ts,data, caplen,vlan);
    }
}
#pragma GCC diagnostic warning "-Wcast-align"
 
 
/************
 *** MMAP ***
 ************/

#ifdef HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif

/**
 * fake implementation of mmap and munmap if we don't have them
 */
#if !defined(HAVE_MMAP)
#define PROT_READ 0
#define MAP_FILE 0
#define MAP_SHARED 0
void *mmap(void *addr,size_t length,int prot, int flags, int fd, off_t offset)
{
    void *buf = (void *)malloc(length);
    if(!buf) return 0;
    read(fd,buf,length);			// should explore return code
    return buf;
}

void munmap(void *buf,size_t size)
{
    free(buf);
}

#endif


#if 0
/* This could be much more efficient */
const uint8_t *find_crlfcrlf(const uint8_t *base,size_t len)
{
    while(len>4){
	if(base[0]=='\r' && base[1]=='\n' && base[2]=='\r' && base[3]=='\n'){
	    return base;
	}
	len--;
	base++;
    }
    return 0;
}



#ifdef HAVE_LIBZ
#define ZLIB_CONST
#ifdef GNUC_HAS_DIAGNOSTIC_PRAGMA
#  pragma GCC diagnostic ignored "-Wundef"
#  pragma GCC diagnostic ignored "-Wcast-qual"
#endif
#ifdef HAVE_ZLIB_H
#include <zlib.h>
#endif
static void process_gzip(class tcpdemux &demux,
			 std::stringstream &ss,
			 const std::string &fname,const unsigned char *base,size_t len)
{
    if((len>4) && (base[0]==0x1f) && (base[1]==0x8b) && (base[2]==0x08) && (base[3]==0x00)){
	size_t uncompr_size = len * 16;
	unsigned char *decompress_buf = (unsigned char *)malloc(uncompr_size);
	if(decompress_buf==0) return;	// too big?

	z_stream zs;
	memset(&zs,0,sizeof(zs));
	zs.next_in = (Bytef *)base; // note that next_in should be typedef const but is not
	zs.avail_in = len;
	zs.next_out = decompress_buf;
	zs.avail_out = uncompr_size;
		
	int r = inflateInit2(&zs,16+MAX_WBITS);
	if(r==0){
	    r = inflate(&zs,Z_SYNC_FLUSH);
	    /* Ignore the error return; process data if we got anything */
	    if(zs.total_out>0){
		demux.write_to_file(ss,fname,sbuf_t(pos0_t(),decompress_buf,zs.total_out,zs.total_out,false));
	    }
	    inflateEnd(&zs);
	}
	free(decompress_buf);
    }
}
#endif
#endif

/** 
 * After the flow is finished, put it in an SBUF and process it.
 * if we are doing post-processing.
 * This is called from tcpip::~tcpip() in tcpip.cpp.
 */
void tcpdemux::post_process_capture_flow(std::stringstream &xmladd,
					 const std::string &flow_pathname)
{
    int fd2 = retrying_open(flow_pathname,O_RDONLY|O_BINARY,0);
    if(fd2<0){
	perror("open");
	return;
    }
    sbuf_t *sbuf = sbuf_t::map_file(flow_pathname,pos0_t(flow_pathname),fd2);
    if(sbuf){
	process_sbuf(scanner_params(scanner_params::scan,*sbuf,*fs,&xmladd));
    }
    ::close(fd2);
}

