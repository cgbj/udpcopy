/*
 *  UDPCopy
 *  An online replication tool for UDP based applications
 *
 *  Copyright 2012 Netease, Inc.  All rights reserved.
 *  Use and distribution licensed under the BSD license.
 *  See the LICENSE file for full text.
 *
 *  Authors:
 *      bin wang <wangbin579@gmail.com>
 */

#include <xcopy.h>
#include <udpcopy.h>

/* Global variables for udpcopy client */
xcopy_clt_settings clt_settings;

static void set_signal_handler(){
    atexit(udp_copy_exit);
    signal(SIGINT,  udp_copy_over);
    signal(SIGPIPE, udp_copy_over);
    signal(SIGHUP,  udp_copy_over);
    signal(SIGTERM, udp_copy_over);
}

static void usage(void) {  
    printf("UDPCopy " VERSION "\n");
    printf("-x <transfer,> what we copy and where send to\n"
           "               transfer format:\n"
           "               online_ip:online_port-target_ip:target_port,...\n"
           "               or :\n"
           "               online_port-target_ip:target_port,...\n");
    printf("-c <ip>        localhost will be changed to this ip address\n"
           "               when sending to another machine\n"
           "               default value is online ip\n");
#if (UDPCOPY_OFFLINE)
    printf("-i <file>      input pcap file(only valid for offline)\n");
#endif
    printf("-n <num>       the number of replication for multi-copying\n"
           "               the less,the better\n"
           "               max value allowed is 1023:\n"
           "-f <num>       port shift factor for mutiple udpcopy instances\n"
           "               max value allowed is 1023:\n");
    printf("-m <num>       max memory to use for udpcopy in megabytes\n"
           "               default value is 512:\n"
           "-M <num>       MTU sent to backend(default:1500)\n"
           "-l <file>      log file path\n");
    printf("-P <file>      save PID in <file>, only used with -d option\n"
           "-h             print this help and exit\n"
           "-v             version\n"
           "-d             run as a daemon\n"
           "-q <qps>       send num per s, else filter\n");
}



static int read_args(int argc, char **argv){
    int  c;
    
    while (-1 != (c = getopt(argc, argv,
         "x:" /* where we copy request from and to */
         "c:" /* localhost will be changed to this ip address */
#if (UDPCOPY_OFFLINE)
         "i:" /* input pcap file */
#endif
         "n:" /* the replicated number of each request for multi-copying */
         "f:" /* port shift factor for mutiple udpcopy instances */
         "m:" /* max memory to use for udpcopy client in megabytes */
         "M:" /* MTU sent to backend */
         "l:" /* error log file path */
         "P:" /* save PID in file */
         "h"  /* help, licence info */   
         "v"  /* verbose */
         "d"  /* daemon mode */
         "q:"  /* qps */
        ))) {
        switch (c) {
            case 'x':
                clt_settings.raw_transfer = optarg;
                break;
            case 'c':
                clt_settings.lo_tf_ip = inet_addr(optarg);
                break;
#if (UDPCOPY_OFFLINE)  
            case 'i':
                clt_settings.pcap_file= optarg;
                break;
#endif
            case 'n':
                clt_settings.replica_num = atoi(optarg);
                break;
            case 'f':
                clt_settings.factor = atoi(optarg);
                break;
            case 'm':
                clt_settings.max_rss = 1024*atoi(optarg);
                break;
            case 'l':
                clt_settings.log_path = optarg;
                break;
            case 'M':
                clt_settings.mtu = atoi(optarg);
                break;
            case 'h':
                usage();
                exit(EXIT_SUCCESS);
            case 'v':
                printf ("udpcopy version:%s\n", VERSION);
                exit(EXIT_SUCCESS);
            case 'd':
                clt_settings.do_daemonize = 1;
                break;
            case 'q':
                clt_settings.qps = atoi(optarg);
                break;
            case 'P':
                clt_settings.pid_file = optarg;
                break;
            default:
                fprintf(stderr, "Illegal argument \"%c\"\n", c);
                exit(EXIT_FAILURE);
        }
    }
    return 0;
}

static void output_for_debug(int argc, char **argv)
{
    /* Print udpcopy version */
    log_info(LOG_NOTICE, "udpcopy version:%s", VERSION);
    /* Print target */
    log_info(LOG_NOTICE, "target:%s", clt_settings.raw_transfer);

#if (UDPCOPY_OFFLINE)
    log_info(LOG_NOTICE, "UDPCOPY_OFFLINE mode");
#endif
}

static void parse_ip_port_pair(char *addr, uint32_t *ip,
        uint16_t *port)
{
    char    *seq, *ip_s, *port_s;
    uint16_t tmp_port;

    if ((seq = strchr(addr, ':')) == NULL) {
        log_info(LOG_NOTICE, "set global port for udpcopy");
        *ip = 0;
        port_s = addr;
    } else {
        ip_s = addr;
        port_s = seq + 1;

        *seq = '\0';
        *ip = inet_addr(ip_s);
        *seq = ':';
    }

    tmp_port = atoi(port_s);
    *port = htons(tmp_port);
}

/*
 * One target format:
 * 192.168.0.1:80-192.168.0.2:8080 
 * or
 * 80-192.168.0.2:8080
 */
static int parse_target(ip_port_pair_mapping_t *ip_port, char *addr)
{
    char   *seq, *addr1, *addr2;

    if ((seq = strchr(addr, '-')) == NULL) {
        log_info(LOG_WARN, "target \"%s\" is invalid", addr);
        return -1;
    } else {
        *seq = '\0';
    }

    addr1 = addr;
    addr2 = seq + 1;

    parse_ip_port_pair(addr1, &ip_port->online_ip, &ip_port->online_port);
    parse_ip_port_pair(addr2, &ip_port->target_ip, &ip_port->target_port);

    if (clt_settings.lo_tf_ip == 0) {
        clt_settings.lo_tf_ip = ip_port->online_ip;
    }

    *seq = '-';

    return 0;
}

/* 
 * Retrieve target addresses
 * Format(by -x argument): 
 * 192.168.0.1:80-192.168.0.2:8080,192.168.0.1:8080-192.168.0.3:80
 */
static int retrieve_target_addresses(char *raw_transfer,
        ip_port_pair_mappings_t *transfer)
{
    int   i;
    char *p, *seq;

    if (raw_transfer == NULL) {
        log_info(LOG_ERR, "it must have -x argument");
        fprintf(stderr, "no -x argument\n");
        return -1;
    }

    for (transfer->num = 1, p = raw_transfer; *p; p++) {
        if (*p == ',') {
            transfer->num++;
        }
    }

    transfer->mappings = malloc(transfer->num *
                                sizeof(ip_port_pair_mapping_t *));
    if (transfer->mappings == NULL) {
        return -1;
    }
    memset(transfer->mappings, 0, 
            transfer->num * sizeof(ip_port_pair_mapping_t *));

    for (i = 0; i < transfer->num; i++) {
        transfer->mappings[i] = malloc(sizeof(ip_port_pair_mapping_t));
        if (transfer->mappings[i] == NULL) {
            return -1;
        }
        memset(transfer->mappings[i], 0, sizeof(ip_port_pair_mapping_t));
    }

    p = raw_transfer;
    i = 0;
    for ( ;; ) {
        if ((seq = strchr(p, ',')) == NULL) {
            parse_target(transfer->mappings[i++], p);
            break;
        } else {
            *seq = '\0';
            parse_target(transfer->mappings[i++], p);
            *seq = ',';

            p = seq + 1;
        }
    }

    return 0;
}

static int sigignore(int sig) 
{    
    struct sigaction sa;

    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;

    if (sigemptyset(&sa.sa_mask) == -1 || sigaction(sig, &sa, 0) == -1){
        return -1;
    }       
    return 0;
}

static int set_details()
{
    int            rand_port;
    struct timeval tp;
    unsigned int   seed;

    /* Generate random port for avoiding port conflicts */
    gettimeofday(&tp, NULL);
    seed = tp.tv_usec;
    rand_port = (int)((rand_r(&seed)/(RAND_MAX + 1.0))*512);
    clt_settings.rand_port_shifted = rand_port;
    /* Set signal handler */    
    set_signal_handler();
    /* Set ip port pair mapping according to settings */
    if (retrieve_target_addresses(clt_settings.raw_transfer,
                              &clt_settings.transfer) == -1)
    {
        exit(EXIT_FAILURE);
    } 

#if (UDPCOPY_OFFLINE)
    if (NULL == clt_settings.pcap_file){
        log_info(LOG_ERR, "it must have -i argument for offline");
        fprintf(stderr, "no -i argument\n");
        exit(EXIT_FAILURE);
    }
#endif


    /* Daemonize */
    if (clt_settings.do_daemonize) {
        if (sigignore(SIGHUP) == -1) {
            perror("Failed to ignore SIGHUP");
            log_info(LOG_ERR, "Failed to ignore SIGHUP");
        }    
        if (daemonize() == -1) {
            fprintf(stderr, "failed to daemon() in order to daemonize\n");
            exit(EXIT_FAILURE);
        }    
    }    
    return 0;
}

/* Set defaults */
static void settings_init()
{
    /* Init values */
    clt_settings.mtu = DEFAULT_MTU;
    clt_settings.max_rss = MAX_MEMORY_SIZE;
    clt_settings.qps = 100000000;
}

/*
 * Main entry point
 */
int main(int argc ,char **argv)
{
    tc_event_loop_t event_loop;

    int ret;

    tc_time_update();
    /* Set defaults */
    settings_init();
    /* Read args */
    read_args(argc, argv);
    /* Init log for outputing debug info */
    log_init(clt_settings.log_path);
    /* Output debug info */
    output_for_debug(argc, argv);
    /* Set details for running */
    set_details();

    ret = tc_event_loop_init(&event_loop, MAX_FD_NUM);
    if (ret == TC_EVENT_ERROR) {
        log_info(LOG_ERR, "event loop init failed, io type:%d",
                 clt_settings.multiplex_io);
        return -1;
    }

    /* Initiate udpcopy client*/
    ret = udp_copy_init(&event_loop);
    if (SUCCESS != ret){
        exit(EXIT_FAILURE);
    }
    /* Run now */
    tc_event_process_cycle(&event_loop);

    return 0;
}

