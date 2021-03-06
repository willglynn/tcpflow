/*
 * This file is part of tcpflow by Simson Garfinkel <simsong@acm.org>.
 * Originally by Jeremy Elson <jelson@circlemud.org>.
 *
 * This source code is under the GNU Public License (GPL) version 3.
 * See COPYING for details.
 *
 */

#define __MAIN_C__

#include "tcpflow.h"
#include "bulk_extractor_i.h"
#include <string>
#include <vector>

#ifdef HAVE_NETINET_IP_H
#include <netinet/ip.h>
#endif

const char *progname = 0;		// name of the program
int debug = DEFAULT_DEBUG_LEVEL;	// global variable, not clear why

/* semaphore prevents multiple copies from outputing on top of each other */
#ifdef HAVE_PTHREAD
#include <semaphore.h>
sem_t *semlock = 0;
#endif

/****************************************************************
 *** SCANNER PLUG-IN SYSTEM 
 ****************************************************************/

scanner_t *scanners_builtin[] = {
    scan_md5,
    scan_http,
    scan_netviz,
    scan_tcpdemux,
    0};

bool opt_no_promisc = false;		// true if we should not use promiscious mode

/****************************************************************
 *** USAGE
 ****************************************************************/

static void usage() __attribute__ ((__noreturn__));
static void usage()
{
    std::cout << PACKAGE << " version " << VERSION << "\n\n";
    std::cout << "usage: " << progname << " [-achpsv] [-b max_bytes] [-d debug_level] [-f max_fds]\n";
    std::cout << "      [-i iface] [-L semlock] [-r file] [-R file] [-o outdir] [-X xmlfile]\n";
    std::cout << "      [-m min_bytes] [-F[ct]] [expression]\n\n";
    std::cout << "   -a: do ALL processing (http expansion, create report.xml, etc.)\n";
    std::cout << "   -b: max number of bytes per flow to save\n";
    std::cout << "   -B: force binary output to console, even with -c or -C\n";
    std::cout << "   -c: console print only (don't create files)\n";
    std::cout << "   -C: console print only, but without the display of source/dest header\n";
    std::cout << "   -d: debug level; default is " << DEFAULT_DEBUG_LEVEL << "\n";
    std::cout << "   -e: output each flow in alternating colors\n";
    std::cout << "   -f: maximum number of file descriptors to use\n";
    std::cout << "   -h: print this help message\n";
    std::cout << "   -i: network interface on which to listen\n";
    std::cout << "   -L  semlock - specifies that writes are locked using a named semaphore\n";
    std::cout << "   -p: don't use promiscuous mode\n";
    std::cout << "   -r: read packets from tcpdump pcap file (may be repeated)\n";
    std::cout << "   -R: read packets from tcpdump pcap file TO FINISH CONNECTIONS\n";
    std::cout << "   -s: strip non-printable characters (change to '.')\n";
    std::cout << "   -v: verbose operation equivalent to -d 10\n";
    std::cout << "   -V: print version number and exit\n";
    std::cout << "   -o  outdir   : specify output directory (default '.')\n";
    std::cout << "   -X  filename : DFXML output to filename\n";
    std::cout << "   -m  bytes    : specifies the minimum number of bytes that a stream may\n";
    std::cout << "  skip before starting a new stream (default "
	      << (unsigned)tcpdemux::options::MAX_SEEK << ").\n";
    std::cout << "   -AH : extract HTTP objects and unzip GZIP-compressed HTTP messages\n";
    std::cout << "   -Fc : append the connection counter to ALL filenames\n";
    std::cout << "   -Ft : prepend the time_t timestamp to ALL filenames\n";
    std::cout << "   -FT : prepend the ISO8601 timestamp to ALL filenames\n";
    std::cout << "   -FX : Do not output any files (other than report files)\n";
    std::cout << "   -FM : Calculate the MD5 for every flow\n";
    std::cout << "   -T<template> : specify an arbitrary filename template (default "
	 << flow::filename_template << ")\n";
    std::cout << "   -Z: do not decompress gzip-compressed HTTP transactions\n";
    info_scanners(false,scanners_builtin,'E','x');
    std::cout << "\n";
    std::cout << "Depricated: (don't use)\n";
    std::cout << "   -P: don't purge tcp connections on FIN (could result in lost data)\n";
    std::cout << "\n";
    std::cout << "expression: tcpdump-like filtering expression\n";
    flow::usage();
    std::cout << "\nSee the man page for additional information.\n\n";
    exit(0);
}

/**
 * Create the dfxml output
 */

static void dfxml_create(class xml &xreport,const std::string &command_line)
{
    xreport.push("dfxml","xmloutputversion='1.0'");
    xreport.push("metadata",
		 "\n  xmlns='http://afflib.org/tcpflow/' "
		 "\n  xmlns:xsi='http://www.w3.org/2001/XMLSchema-instance' "
		 "\n  xmlns:dc='http://purl.org/dc/elements/1.1/'" );
    xreport.xmlout("dc:type","Feature Extraction","",false);
    xreport.pop();
    xreport.add_DFXML_creator(PACKAGE_NAME,PACKAGE_VERSION,"",command_line);
    xreport.push("configuration");
    xreport.pop();			// configuration
}


/* String replace. Perhaps not the most efficient, but it works */
void replace(std::string &str,const std::string &from,const std::string &to)
{
    if(from.size()==0) return;
    bool changed = false;

    std::stringstream ss;
    for(unsigned int i=0;i<str.size();){
	if(str.substr(i,from.size())==from){
	    ss << to;
	    i+=from.size();
	    changed = true;
	} else {
	    ss << str.at(i);
	    i++;
	}
    }
    if(changed) str = ss.str();			// copy over original
}

/* These must be global variables so they are available in the signal handler */
feature_recorder_set *the_fs = 0;
xml *xreport = 0;
void terminate(int sig)
{
    DEBUG(1) ("terminating");

    phase_shutdown(*the_fs,*xreport);	// give plugins a chance to do a clean shutdown
    exit(0); /* libpcap uses onexit to clean up */
}


/*
 * process an input file or device
 * May be repeated.
 * If start is false, do not initiate new connections
 */
static void process_infile(const std::string &expression,const char *device,const std::string &infile)
{
    char error[PCAP_ERRBUF_SIZE];
    pcap_t *pd=0;
    int dlt=0;
    pcap_handler handler;

    if (infile!=""){
	if ((pd = pcap_open_offline(infile.c_str(), error)) == NULL){	/* open the capture file */
	    die("%s", error);
	}
	dlt = pcap_datalink(pd);	/* get the handler for this kind of packets */
	handler = find_handler(dlt, infile.c_str());
    } else {
	/* if the user didn't specify a device, try to find a reasonable one */
	if (device == NULL){
	    if ((device = pcap_lookupdev(error)) == NULL){
		die("%s", error);
	    }
	}

	/* make sure we can open the device */
	if ((pd = pcap_open_live(device, SNAPLEN, !opt_no_promisc, 1000, error)) == NULL){
	    die("%s", error);
	}
#if defined(HAVE_SETUID) && defined(HAVE_GETUID)
	/* drop root privileges - we don't need them any more */
	if(setuid(getuid())){
	    perror("setuid");
	}
#endif
	/* get the handler for this kind of packets */
	dlt = pcap_datalink(pd);
	handler = find_handler(dlt, device);
    }

#if 0
    if(getenv("TCPFLOW_MFS")) {
        // wrap the handler so that plugins through the PCB interface will be called
        pcb::init(handler, true);
        // currently no non-default plugins are loaded, so do startup right away
        pcb::do_startup();
        handler = &pcb::handle;
    }
#endif

    /* If DLT_NULL is "broken", giving *any* expression to the pcap
     * library when we are using a device of type DLT_NULL causes no
     * packets to be delivered.  In this case, we use no expression, and
     * print a warning message if there is a user-specified expression
     */
#ifdef DLT_NULL_BROKEN
    if (dlt == DLT_NULL && expression != ""){
	DEBUG(1)("warning: DLT_NULL (loopback device) is broken on your system;");
	DEBUG(1)("         filtering does not work.  Recording *all* packets.");
    }
#endif /* DLT_NULL_BROKEN */

    DEBUG(20) ("filter expression: '%s'",expression.c_str());

    /* install the filter expression in libpcap */
    struct bpf_program	fcode;
    if (pcap_compile(pd, &fcode, expression.c_str(), 1, 0) < 0){
	die("%s", pcap_geterr(pd));
    }

    if (pcap_setfilter(pd, &fcode) < 0){
	die("%s", pcap_geterr(pd));
    }

    /* initialize our flow state structures */

    /* set up signal handlers for graceful exit (pcap uses onexit to put
     * interface back into non-promiscuous mode
     */
    portable_signal(SIGTERM, terminate);
    portable_signal(SIGINT, terminate);
#ifdef SIGHUP
    portable_signal(SIGHUP, terminate);
#endif

    /* start listening or reading from the input file */
    if (infile == "") DEBUG(1) ("listening on %s", device);
    if (pcap_loop(pd, -1, handler, (u_char *)tcpdemux::getInstance()) < 0){
	die("%s", pcap_geterr(pd));
    }
}




int main(int argc, char *argv[])
{
    bool force_binary_output = false;
    bool opt_all = true;
    char *device = NULL;
    const char *lockname = 0;
    int need_usage = 0;
    std::string reportfilename;
    std::vector<std::string> Rfiles;	// files for finishing
    std::vector<std::string> rfiles;	// files to read
    tcpdemux &demux = *tcpdemux::getInstance();			// the demux object we will be using.
    std::string command_line = xml::make_command_line(argc,argv);

    /* Set up debug system */
    progname = argv[0];
    init_debug(argv);

    /* Make sure that the system was compiled properly */
    if(sizeof(struct ip)!=20 || sizeof(struct tcphdr)!=20){
	fprintf(stderr,"COMPILE ERROR.\n");
	fprintf(stderr,"  sizeof(struct ip)=%d; should be 20.\n", (int)sizeof(struct ip));
	fprintf(stderr,"  sizeof(struct tcphdr)=%d; should be 20.\n", (int)sizeof(struct tcphdr));
	fprintf(stderr,"CANNOT CONTINUE\n");
	exit(1);
    }

    int arg;
    while ((arg = getopt(argc, argv, "aA:Bb:cCd:eE:F:f:Hhi:L:m:o:PpR:r:sT:Vvx:X:Z")) != EOF) {
	switch (arg) {
	case 'a':
	    demux.opt.opt_after_header = true;
	    demux.opt.opt_md5 = true;
	    scanners_enable_all();
	    opt_all = true;
	    continue;
	    
	case 'A': 
	    for(const char *cc=optarg;*cc;cc++){
		switch(*cc){
		case 'H': demux.opt.opt_after_header = true;break;
		default:
		    fprintf(stderr,"-A invalid after processing '%c'\n",*cc);
		    need_usage=true;
		}
	    }
	    break;
	case 'b':
	    demux.opt.max_bytes_per_flow = atoi(optarg);
	    if(debug > 1) {
		std::cout << "capturing max of " << demux.opt.max_bytes_per_flow << " bytes per flow." << std::endl;
	    }
	    break;
	case 'B':
	    force_binary_output = true; DEBUG(10) ("force binary output");
	    break;
	case 'C':
	    demux.opt.console_output = true;	DEBUG(10) ("printing packets to console only");
	    demux.opt.suppress_header = 1;	DEBUG(10) ("packet header dump suppressed");
	    demux.opt.strip_nonprint = 1;	DEBUG(10) ("converting non-printable characters to '.'");
	    break;
	case 'c':
	    demux.opt.console_output = true;	DEBUG(10) ("printing packets to console only");
	    demux.opt.strip_nonprint = 1;	DEBUG(10) ("converting non-printable characters to '.'");
	    break;
	case 'd':
	    if ((debug = atoi(optarg)) < 0) {
		debug = DEFAULT_DEBUG_LEVEL;
		DEBUG(1) ("warning: -d flag with 0 debug level '%s'", optarg);
	    }
	    break;
	case 'e':
	    demux.opt.use_color  = 1;
	    DEBUG(10) ("using colors");
	    break;
        case 'E': scanners_enable(optarg); break;
	case 'F':
	    for(const char *cc=optarg;*cc;cc++){
		switch(*cc){
		case 'c': replace(flow::filename_template,"%c","%C"); break;
		case 't': flow::filename_template = "%tT" + flow::filename_template; break;
		case 'T': flow::filename_template = "%T"  + flow::filename_template; break;
		case 'X': demux.opt.opt_output_enabled = false;break;
		case 'M': demux.opt.opt_md5 = true;break;
		default:
		    fprintf(stderr,"-F invalid format specification '%c'\n",*cc);
		    need_usage = true;
		}
	    }
	    break;
	case 'f':
	    if ((demux.opt.max_desired_fds = atoi(optarg)) < (NUM_RESERVED_FDS + 2)) {
		DEBUG(1) ("warning: -f flag must be used with argument >= %d",
			  NUM_RESERVED_FDS + 2);
		demux.opt.max_desired_fds = 0;
	    }
	    break;
	case 'i': device = optarg; break;
	case 'L': lockname = optarg; break;
	case 'm':
	    demux.opt.max_seek = atoi(optarg);
	    DEBUG(10) ("max_seek set to %d",demux.opt.max_seek); break;
	case 'o': demux.outdir = optarg; break;
	case 'P': demux.opt.opt_no_purge = true; break;
	case 'p': opt_no_promisc = true; DEBUG(10) ("NOT turning on promiscuous mode"); break;
	case 'R': Rfiles.push_back(optarg); break;
	case 'r': rfiles.push_back(optarg); break;
	case 's': demux.opt.strip_nonprint = 1;
	    DEBUG(10) ("converting non-printable characters to '.'"); break;
	case 'T': flow::filename_template = optarg;break;
	case 'V': std::cout << PACKAGE << " " << PACKAGE_VERSION << "\n"; exit (1);
	case 'v': debug = 10; break;
	case 'x': scanners_disable(optarg);break;
	case 'X': reportfilename = optarg;break;
	case 'Z': demux.opt.opt_gzip_decompress = 0; break;
	case 'H': info_scanners(true,scanners_builtin,'E','x'); exit(0);
	case 'h': case '?': usage(); break;
	default:
	    DEBUG(1) ("error: unrecognized switch '%c'", optopt);
	    need_usage = 1;
	    break;
	}
    }
    argc -= optind;
    argv += optind;

    if(getenv("TCPFLOW_MFS")) scanners_enable("pcapviz");    /* Special code for Mike */

    /* Load all the scanners and enable the ones we care about */
    if(demux.opt.opt_md5) scanners_enable("md5");
    load_scanners(scanners_builtin);
    scanners_process_commands();

    if( (opt_all) && (reportfilename.size()==0) ){
	reportfilename = demux.outdir + "/report.xml";
    }

    /* print help and exit if there was an error in the arguments */
    if (need_usage) {
	usage();
	exit(1);
    }

    /* get the user's expression out of remainder of the arg... */
    std::string expression = "";
    for(int i=0;i<argc;i++){
	if(expression.size()>0) expression+=" ";
	expression += argv[i];
    }

    struct stat sbuf;
    if(lockname){
#if defined(HAVE_SEMAPHORE_H) && defined(HAVE_PTHREAD)
	semlock = sem_open(lockname,O_CREAT,0777,1); // get the semaphore
#else
	fprintf(stderr,"%s: attempt to create lock pthreads not present\n",argv[0]);
	exit(1);
#endif	
    }

    if(force_binary_output){
	demux.opt.strip_nonprint = false;
    }

    /* make sure outdir is a directory. If it isn't, try to make it.*/
    if(stat(demux.outdir.c_str(),&sbuf)==0){
	if(!S_ISDIR(sbuf.st_mode)){
	    std::cerr << "outdir is not a directory: " << demux.outdir << "\n";
	    exit(1);
	}
    } else {
	if(MKDIR(demux.outdir.c_str(),0777)){
	    std::cerr << "cannot create " << demux.outdir << ": " << strerror(errno) << "\n";
	    exit(1);
	}
    }

    if(reportfilename.size()>0){
	xreport = new xml(reportfilename,false);
	dfxml_create(*xreport,command_line);
	demux.xreport = xreport;
    }

    argc -= optind;
    argv += optind;

    DEBUG(10) ("%s version %s ", PACKAGE, VERSION);

    std::string image_fname;		// input filename?
    feature_file_names_t feature_file_names;
    enable_feature_recorders(feature_file_names);
    feature_recorder_set fs(feature_file_names,image_fname,demux.outdir,false);
    the_fs   = &fs;
    demux.fs = &fs;

    if(rfiles.size()==0 && Rfiles.size()==0){
	/* live capture */
#if defined(HAVE_SETUID) && defined(HAVE_GETUID)
        /* Since we don't need network access, drop root privileges */
	if(setuid(getuid())){
	    perror("setuid");
	}
#endif
	demux.start_new_connections = true;
        process_infile(expression,device,"");
    }
    else {
	/* first pick up the new connections with -r */
	demux.start_new_connections = true;
	for(std::vector<std::string>::const_iterator it=rfiles.begin();it!=rfiles.end();it++){
	    process_infile(expression,device,*it);
	}
	/* now pick up the outstanding connection with -R, but don't start new connections */
	demux.start_new_connections = false;
	for(std::vector<std::string>::const_iterator it=Rfiles.begin();it!=Rfiles.end();it++){
	    process_infile(expression,device,*it);
	}
    }

    /* -1 causes pcap_loop to loop forever, but it finished when the input file is exhausted. */

    DEBUG(2)("Open FDs at end of processing:      %d",(int)demux.openflows.size());
    DEBUG(2)("Flow map size at end of processing: %d",(int)demux.flow_map.size());

    demux.close_all();
    phase_shutdown(fs,*xreport);
    
    /*
     * Note: funny formats below are a result of mingw problems with PRId64.
     */
    const std::string total_flow_processed("Total flows processed: %"PRId64);
    const std::string total_packets_processed("Total packets processed: %"PRId64);
    
    DEBUG(2)(total_flow_processed.c_str(),demux.flow_counter);
    DEBUG(2)(total_packets_processed.c_str(),demux.packet_counter);

    if(xreport){
	demux.flow_map_clear();	// empty the map to capture the state
	xreport->add_rusage();
	xreport->pop();			// bulk_extractor
	xreport->close();
	delete xreport;		// causes crash on windows with mingw32
    }
    exit(0);			// return(0) causes crash on Windows
}
