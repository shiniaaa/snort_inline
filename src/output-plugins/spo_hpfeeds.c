/*
** Copyright (C) 2014 Cisco and/or its affiliates. All rights reserved.
** Copyright (C) 2002-2013 Sourcefire, Inc.
** Copyright (C) 1998-2002 Martin Roesch <roesch@sourcefire.com>
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License Version 2 as
** published by the Free Software Foundation.  You may not use, modify or
** distribute this program under any other version of the GNU General
** Public License.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

/* $Id$ */

/* spo_hpfeeds
 *
 * Purpose:  output plugin for hpfeeds publishing
 *
 * Arguments:
 *
 * Effect:
 *
 * Comments:
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <netinet/in.h>
#include <string.h>

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <jansson.h>
#include <hpfeeds.h>
#include <poll.h>
#include <fcntl.h>
#include <pthread.h>
#endif /* !WIN32 */

#ifdef HAVE_STRINGS_H
#include <string.h>
#endif

#include "spo_hpfeeds.h"
#include "event.h"
#include "decode.h"
#include "plugbase.h"
#include "spo_plugbase.h"
#include "parser.h"
#include "snort_debug.h"
#include "mstring.h"
#include "log.h"
#include "util.h"

#include "sp_pattern_match.h"
#include "sp_replace.h"
#include "snort_bounds.h"
#include "snort.h"

#include "pcap.h"
#include <sys/stat.h>

#include "sfutil/sf_textlog.h"
#include "log_text.h"

#define DEFAULT_PORT 10000
#define BUF_LEN 32
#define READ_BLOCK_SIZE 32767

/* hpfeeds state machine */

typedef enum {
    S_INIT,
    S_AUTH,
    S_AUTH_DONE,
    S_ERROR,
    S_TERMINATE
} hpfeeds_session_state_t;

#define HPFEEDS_NOK 0
#define HPFEEDS_CONFIG_SET 1
#define HPFEEDS_AUTH_DONE 2
#define HPFEEDS_READY 3

struct _HPFeedsConfig
{
    /* hpfeeds connection parameter */
    char *hpfeeds_host;
    char *hpfeeds_ident;
    char *hpfeeds_secret;
    char *hpfeeds_channel;
    int hpfeeds_port;
    int reconnect;

    /* socket */
    int sock;
    struct pollfd pfd;

    /* Connection status */
    int status;
};

typedef struct _HPFeedsConfig HPFeedsConfig;

static struct sockaddr_in host;

char server_host[100][512];
char replace_host[100][512];
char buffer[512];
char *t[2];
int line = 0;

#ifndef WIN32

/* list of function prototypes for this preprocessor */
static void AlertHPFeedsInit(struct _SnortConfig *sc, char *args);
static HPFeedsConfig *AlertHPFeedsParseConfig(struct _SnortConfig *sc, char *args);
static void AlertHPFeedsCleanExit(int signal, void *arg);
static void HPFeedsAlert(Packet *, char *, void *, Event *);

void HPFeedsPublish(json_t *json, HPFeedsConfig *config);
void HPFeedsConnect(HPFeedsConfig *config, int reconnect);

#endif

/*
 * Function: AlertHPFeedsSetup()
 *
 * Purpose: Registers the output plugin keyword and initialization
 *          function into the output plugin list.  This is the function that
 *          gets called from InitOutputPlugins() in plugbase.c.
 *
 * Arguments: None.
 *
 * Returns: void function
 *
 */

void AlertHPFeedsSetup(void)
{

#ifndef WIN32

    RegisterOutputPlugin("log_hpfeeds", OUTPUT_TYPE_FLAG__LOG, AlertHPFeedsInit);

    DEBUG_WRAP(DebugMessage(DEBUG_INIT, "Output plugin: log_hpfeeds is setup...\n"););

#else

/* !!!!! hpfeeds is not supported on win32 platform */

#endif
}

#ifndef WIN32

#define RULES_UPDATE_PORT 9999
#define LENGTH_OF_LISTEN_QUEUE 1
#define BUFFER_SIZE 1024

static void RuleUpdateThread(void)
{
    struct sockaddr_in local_addr;
    bzero(&local_addr, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htons(INADDR_ANY);
    local_addr.sin_port = htons(RULES_UPDATE_PORT);

    int local_socket = socket(PF_INET, SOCK_STREAM, 0);
    if (local_socket < 0)
    {
        printf("Create Socket Failed!\n");
        return;
    }

    if (bind(local_socket, (struct sockaddr *)&local_addr, sizeof(local_addr)))
    {
        printf("Server Bind Port: %d Failed!\n", RULES_UPDATE_PORT);
        return;
    }

    if (listen(local_socket, LENGTH_OF_LISTEN_QUEUE))
    {
        printf("Server Listen Failed!\n");
        return;
    }

    while (1)
    {
        LogMessage("Checking for updating rules\n");
        struct sockaddr_in remote_addr;
        socklen_t length = sizeof(remote_addr);

        int remote_socket = accept(local_socket, (struct sockaddr *)&remote_addr, &length);
        if (remote_socket < 0)
        {
            LogMessage("Server Accept Failed!\n");
            continue;
        }

        LogMessage("Recieving Rules Update Notification!\n");

        char buffer[BUFFER_SIZE];
        bzero(buffer, sizeof(buffer));
        length = recv(remote_socket, buffer, BUFFER_SIZE, 0);
        if (length < 0)
        {
            LogMessage("Recieve Data Failed!\n");
            continue;
        }

        if (strcmp(buffer, "Rulesupdate") != 0)
        {
            LogMessage("Unknown message\n");
            continue;
        }

        LogMessage("Updating Rules\n");
        system("/opt/mhn/rules/update_snort_rules.sh");
    }
    /**
    while (1)
    {
        LogMessage("Checking for updating rules\n");
        sleep(5);
    }
**/
}

/*
 * Function: AlertHPFeedsInit(char *)
 *
 * Purpose: Calls the argument parsing function, performs final setup on data
 *          structs, links the preproc function into the function list.
 *
 * Arguments: args => ptr to argument string
 *
 * Returns: void function
 *
 */

static void AlertHPFeedsInit(struct _SnortConfig *sc, char *args)
{

    int i = 0;
    FILE *fp = fopen("/opt/snort_inline/etc/IP_List.txt", "r");

    while (fgets(buffer, sizeof(buffer), fp) != NULL)
    {
        int in = 0;
        char *buf = buffer;
        while ((t[in] = strtok(buf, "-")) != NULL)
        {
            in++;
            buf = NULL;
        }
        strcpy(server_host[i], t[0]);
        strcpy(replace_host[i], t[1]);
	//change /n to /0 in the list
        int a1,a2,L,n=0;
        L = strlen(server_host[i]);
        for (a1=0;a1<=L-n;a1++) 
            if(server_host[i][a1] == '\n') {
                server_host[i][a1] = 0;
            }
        a1,a2,L,n=0;
        L = strlen(replace_host[i]);
        for (a1=0;a1<=L-n;a1++) 
        if(replace_host[i][a1] == '\n') {
            replace_host[i][a1] = 0;
		
        }
        i++;
        line++;
    }
    fclose(fp);

    HPFeedsConfig *config;
    pthread_t thread;

    DEBUG_WRAP(DebugMessage(DEBUG_INIT, "Output: hpfeeds Initialized\n"););

    config = AlertHPFeedsParseConfig(sc, args);

    if (config->status == HPFEEDS_CONFIG_SET)
        HPFeedsConnect(config, 0);

    AddFuncToCleanExitList(AlertHPFeedsCleanExit, config);
    AddFuncToOutputList(sc, HPFeedsAlert, OUTPUT_TYPE__ALERT, config);
    /*
    if (pthread_create(&thread, NULL, RuleUpdateThread, NULL) != 0)
    {
        LogMessage("Failed to create RuleUPdateThread!\n");
    }
    else
    {
        LogMessage("Successfully created RuleUpdateThread!\n");
    }
    */
}

/*
 * Function: AlertHPFeedsParseArgs(char *)
 *
 * Purpose: Read hpfeeds parameters
 *
 * Arguments: args => argument list
 *
 * Returns: void function
 *
 */

static HPFeedsConfig *AlertHPFeedsParseConfig(struct _SnortConfig *sc, char *args)
{
    HPFeedsConfig *cnf = (HPFeedsConfig *)SnortAlloc(sizeof(HPFeedsConfig));
    memset(cnf, '\0', sizeof(HPFeedsConfig));

    cnf->hpfeeds_port = DEFAULT_PORT;
    cnf->sock = -1;
    cnf->status = HPFEEDS_NOK;
    cnf->reconnect = 0;

    if (args != NULL)
    {
        char **toks;
        int num_toks;

        toks = mSplit((char *)args, ",", 6, &num_toks, '\\');

        int count = 0;
        for (; count < num_toks; count++)
        {
            char **stoks;
            int num_stoks;
            char *index = toks[count];

            while (isspace((int)*index))
                ++index;

            if (strcasecmp("reconnect", index) == 0)
            {
                cnf->reconnect = 1;
                continue;
            }

            stoks = mSplit(index, " \t", 2, &num_stoks, 0);

            if (strcasecmp("host", stoks[0]) == 0)
            {
                if (num_stoks > 1 && cnf->hpfeeds_host == NULL)
                    cnf->hpfeeds_host = SnortStrdup(stoks[1]);
                else
                    FatalError("Argument Error in %s(%i): %s\n",
                               file_name, file_line, index);
            }

            else if (strcasecmp("ident", stoks[0]) == 0)
            {
                if (num_stoks > 1 && cnf->hpfeeds_ident == NULL)
                    cnf->hpfeeds_ident = SnortStrdup(stoks[1]);
                else
                    FatalError("Argument Error in %s(%i): %s\n",
                               file_name, file_line, index);
            }

            else if (strcasecmp("secret", stoks[0]) == 0)
            {
                if (num_stoks > 1 && cnf->hpfeeds_secret == NULL)
                    cnf->hpfeeds_secret = SnortStrdup(stoks[1]);
                else
                    FatalError("Argument Error in %s(%i): %s\n",
                               file_name, file_line, index);
            }

            else if (strcasecmp("channel", stoks[0]) == 0)
            {
                if (num_stoks > 1 && cnf->hpfeeds_channel == NULL)
                    cnf->hpfeeds_channel = SnortStrdup(stoks[1]);
                else
                    FatalError("Argument Error in %s(%i): %s\n",
                               file_name, file_line, index);
            }

            else if (strcasecmp("port", stoks[0]) == 0)
            {
                char *end;

                if (num_stoks > 1)
                {
                    cnf->hpfeeds_port = SnortStrtoul(stoks[1], &end, 10);

                    if ((stoks[1] == end) || (errno == ERANGE))
                    {
                        FatalError("Argument Error in %s(%i): %s\n",
                                   file_name, file_line, index);
                    }
                }
                else
                {
                    FatalError("Argument Error in %s(%i): %s\n",
                               file_name, file_line, index);
                }
            }

            mSplitFree(&stoks, num_stoks);
        }

        mSplitFree(&toks, num_toks);
    }

    if ((cnf->hpfeeds_host && cnf->hpfeeds_ident && cnf->hpfeeds_secret && cnf->hpfeeds_channel))
        cnf->status = HPFEEDS_CONFIG_SET;

    return cnf;
}

/*
 * Function: AlertPFeedsCleanExitFunc()
 *
 * Purpose: Cleanup at exit time
 *
 * Arguments: signal => signal that caused this event
 *            arg => data ptr to reference this plugin's data
 *
 * Returns: void function
 */

static void AlertHPFeedsCleanExit(int signal, void *arg)
{
    HPFeedsConfig *config = (HPFeedsConfig *)arg;

    DEBUG_WRAP(DebugMessage(DEBUG_FLOW, "hpfeeds: CleanExit\n"););

    /* free up initialized memory */

    if (config != NULL)
    {
        if (config->hpfeeds_host != NULL)
            free(config->hpfeeds_host);

        if (config->hpfeeds_ident != NULL)
            free(config->hpfeeds_ident);

        if (config->hpfeeds_secret != NULL)
            free(config->hpfeeds_secret);

        if (config->hpfeeds_channel != NULL)
            free(config->hpfeeds_channel);

        if (config->sock != -1)
            close(config->sock);

        free(config);
    }
}

/*
 *
 * Function: pcap_file_send(char* file_name, FILE* fp)
 *
 * Purpose: Send pcap file to server
 *
 * Arguments:         fp => handle of the file to be send
 *             file_name => path and name of the pcap file           
 *
 * Returns: 0  as  OK
 *
 */

#define FILE_SERVER_PORT 6666
#define BUFFER_SIZE 1024

//Transmit content
#define TRANS_FILE_PATH 1
#define TRANS_FILE_CONTENT 2

//Transmit status
#define STATUS_OK 1
#define STATUS_FAIL 2

static int pcap_file_send(char *file_name, FILE *fp)
{
    int length;

    struct sockaddr_in client_addr;
    bzero(&client_addr, sizeof(client_addr));
    client_addr.sin_family = AF_INET;
    client_addr.sin_addr.s_addr = htons(INADDR_ANY);
    client_addr.sin_port = htons(0);

    struct sockaddr_in server_addr = host;
    server_addr.sin_port = htons(FILE_SERVER_PORT);

    int client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket < 0)
    {
        LogMessage("Create Socket Failed!\n");
        return -1;
    }

    if (bind(client_socket, (struct sockaddr *)&client_addr, sizeof(client_addr)))
    {
        LogMessage("Client Bind Port Failed!\n");
        close(client_socket);
        return -1;
    }

    if (connect(client_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        LogMessage("Can Not Connect To Server!\n");
        close(client_socket);
        return -1;
    }

    //First step, send file path to server
    char buffer[BUFFER_SIZE];
    bzero(buffer, sizeof(buffer));
    buffer[0] = TRANS_FILE_PATH;
    if (strlen(file_name) > (BUFFER_SIZE - 1))
    {
        LogMessage("File path too long\n");
        close(client_socket);
        return -1;
    }
    strncpy(&buffer[1], file_name, strlen(file_name));

    send(client_socket, buffer, strlen(file_name) + 1, 0);

    //Then receive transmit status from server
    bzero(buffer, sizeof(buffer));

    if ((length = recv(client_socket, buffer, BUFFER_SIZE, 0)) < 0)
    {
        LogMessage("Recieve Data From Server Failed!\n");
        close(client_socket);
        return -1;
    }

    if (buffer[0] != STATUS_OK)
    {
        LogMessage("Server failed to receive file path: %s\n", &buffer[1]);
        close(client_socket);
        return -1;
    }

    //Second step, send file content to server
    bzero(buffer, BUFFER_SIZE);
    int file_block_length = 0;
    while ((file_block_length = fread(&buffer[1], sizeof(char), BUFFER_SIZE - 1, fp)) > 0)
    {
        buffer[0] = TRANS_FILE_CONTENT;
        //LogMessage("file_block_length = %d\n", file_block_length);

        if (send(client_socket, buffer, file_block_length + 1, 0) < 0)
        {
            LogMessage("Send File Failed!\n");
            close(client_socket);
            return -1;
        }

        bzero(buffer, sizeof(buffer));
    }

    //Sending process over
    LogMessage("Sent file %s to server %x success\n", file_name, (unsigned long)(host.sin_addr.s_addr));
    close(client_socket);
    return 0;
}

/*
 *
 * Function: log_pcap_file(Packet* p, char* file_name)
 *
 * Purpose: Log the packet to a pcap file
 *
 * Arguments:          p => packet. (could be NULL)
 *             file_name => path and name of the pcap file           
 *
 * Returns: 0  as  OK
 *
 */
#define LOG_ERR_PRINT LogMessage

static int log_pcap_file(Packet *p, char *file_name)
{
    pcap_t *pcap_handle;
    pcap_dumper_t *pcap_dump_handle;
    FILE *fp;
    //char* file_path;
    if ((p == NULL) || (p->eh == NULL) || (p->iph == NULL))
    {
        LOG_ERR_PRINT("This is not a Eth/IPV4 frame\n");
        return -1;
    }

    if ((pcap_handle = pcap_open_dead(DLT_EN10MB, 65535)) == NULL)
    {
        LOG_ERR_PRINT("Failed to create pcap handler\n");
        return -2;
    }

    if ((pcap_dump_handle = pcap_dump_open(pcap_handle, file_name)) == NULL)
    {
        LOG_ERR_PRINT("Failed to open dump file: %s\n", pcap_geterr(pcap_handle));
        free(file_name);
        return -4;
    }

    pcap_dump((u_char *)pcap_dump_handle, (struct pcap_pkthdr *)p->pkth, p->pkt);
    pcap_dump_flush(pcap_dump_handle);
    pcap_dump_close(pcap_dump_handle);
    pcap_close(pcap_handle);

    fp = fopen(file_name, "rb");
    pcap_file_send(&file_name[10], fp);
    fclose(fp);

    return 0;
}

/*
 *
 * Function: HPFeedsAlert(Packet *, char *, FILE *, char *, numargs const int)
 *
 * Purpose: Write a user defined message
 *
 * Arguments:     p => packet. (could be NULL)
 *              msg => the message to send
 *             args => hpfeeds configuration
 *
 * Returns: void function
 *
 */

static void HPFeedsAlert(Packet *p, char *msg, void *arg, Event *event)
{
    HPFeedsConfig *config = (HPFeedsConfig *)arg;
    char *file_name;
    char *path;
    char *hpfeeds_path;
    int rc;

    /*
    char server_host[100][512];
    char honeypot_host[100][512];
    char buffer[512];

    int i=0;
    char t[2];
	int line=0;
    FILE *fp = fopen("/home/tinemar/IP_List.txt","r");

    while(fgets(buffer,sizeof(buffer),fp)!=NULL){
	    int in=0;
    	char *buf=buffer;
        while((t[in]=strtok(buf,"-"))!=NULL){
            in++;
            buf=NULL;
        }
        strcpy(server_host[i],t[0]);
        strcpy(honeypot_host[i],t[1]);
	    LOG_ERR_PRINT("%s,%s\n",server_host[i],honeypot_host[i]);
        i++;
		line++;
    }
    
    fclose(fp);
    */
    /*
    int j=0;
    in_addr_t server_ip_fake;
    in_addr_t honeypot_ip_fake;
    for(j=0;j<line;j++){
	server_ip_fake=inet_addr(server_ip[j]);
	honeypot_ip_fake=inet_addr(honeypot_ip[j]);
	//LOG_ERR_PRINT("%s,%s\n",server_ip[j],honeypot_ip[j]);
	//LOG_ERR_PRINT("%d,%d\n",server_ip_fake,honeypot_ip_fake);
	//LOG_ERR_PRINT("%d\n",p->iph->ip_dst.s_addr);
        if(p->iph->ip_dst.s_addr==server_ip_fake){
	    
            p->iph->ip_dst.s_addr=honeypot_ip_fake;
            Encode_Update(p);
        }
  
    }

    in_addr_t dstip1;
    dstip1=inet_addr("192.168.101.147");
    p->iph->ip_dst.s_addr = dstip1;
    
    //p->packet_flags |= PKT_MODIFIED; 8.3
    Encode_Update(p);

*/
    //char *host_replace;
    char *host_start;
    //host_replace = "Host: 192.168.101.111";
    /*
    host_start = strstr(p->data, "Host: 192.168.101.147");
    if(host_start!=NULL){
        SafeMemcpy(host_start, host_replace, strlen(host_replace), p->data, (p->dsize + p->data));    
        Encode_Update(p);    
    }
    */
    //LOG_ERR_PRINT("%d,%s\n",1,p->data);

    int j = 0;
    for (j = 0; j < line; j++)
    {
        host_start = strstr(p->data, server_host[j]);
        if(host_start!=NULL){
            LOG_ERR_PRINT("%d,%s\n",1,p->data);
            SafeMemcpy(host_start, replace_host[j], strlen(replace_host[j]), p->data, (p->dsize + p->data));
	    LOG_ERR_PRINT("%d,%s\n",2,p->data);
            Encode_Update(p);
	    //LOG_ERR_PRINT("%d,%s\n",3,p->data);
        }
    }

    if (p == NULL)
        return;

    /* We don't have open socket */
    if (config->sock == -1)
        return;

    json_t *json_record = json_object();
    json_object_set_new(json_record, "sensor", json_string((char *)config->hpfeeds_ident));

    char timestamp[TIMEBUF_SIZE];
    struct tm *lt = localtime(&p->pkth->ts.tv_sec);

    strftime((char *)timestamp, 64, "%Y/%m/%d %H:%M:%S", lt);

    char timestamp_usec[68];
    snprintf(timestamp_usec, 68, "%s.%d", timestamp, (int)p->pkth->ts.tv_usec);

    json_object_set_new(json_record, "timestamp", json_string((char *)timestamp_usec));

    char construct_buf[BUF_LEN];

    /* EVENT */
    if (event != NULL)
    {
        snprintf(construct_buf, BUF_LEN, "%d:%d:%d", event->sig_generator, event->sig_id, event->sig_rev);
        json_object_set_new(json_record, "header", json_string(construct_buf));
        json_object_set_new(json_record, "classification", json_integer(event->classification));
        json_object_set_new(json_record, "priority", json_integer(event->priority));
    }
    else
    {
        json_object_set_new(json_record, "header", json_string("null"));
        json_object_set_new(json_record, "classification", json_integer(-1));
        json_object_set_new(json_record, "priority", json_integer(-1));
    }

    json_object_set_new(json_record, "signature", json_string((char *)msg ? msg : "null"));

    /* IP */
    if (IPH_IS_VALID(p))
    {
        path = malloc(1000);

        //if "/var/pcap" directory  is changed, "&file_name[10]" in this function and log_pcap_file function should also be changed
        strftime(path, 1000, "/var/pcap/%Y-%m-%d", localtime(&p->pkth->ts.tv_sec));
        if ((access(path, 0)) != 0)
        {
            if ((mkdir(path, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH)) != 0)
            {
                LOG_ERR_PRINT("Failed to make directionary\n");
                return;
            }
        }

        file_name = malloc(1000);
        snprintf(file_name, 1000, "%s/%lx%lx%lx%lx.pcap", path, ntohl((uint32_t)(p->iph->ip_src.s_addr)), ntohl((uint32_t)(p->iph->ip_dst.s_addr)), p->pkth->ts.tv_sec, p->pkth->ts.tv_usec);
/*
        if ((rc = log_pcap_file(p, file_name)) == 0)
        {
            json_object_set_new(json_record, "file_path", json_string((char *)(&file_name[10])));
        }
*/
        if (file_name != NULL)
        {
            free(file_name);
        }

        if (path != NULL)
        {
            free(path);
        }

        switch (GET_IPH_PROTO(p))
        {
        case IPPROTO_UDP:
            json_object_set_new(json_record, "proto", json_string((char *)"UDP"));
            break;
        case IPPROTO_TCP:
            json_object_set_new(json_record, "proto", json_string((char *)"TCP"));
            break;
        case IPPROTO_ICMP:
            json_object_set_new(json_record, "proto", json_string((char *)"ICMP"));
            break;
        default:
            json_object_set_new(json_record, "proto", json_string((char *)"???"));
            break;
        };

        switch (GET_IPH_PROTO(p))
        {
        case IPPROTO_UDP:
        case IPPROTO_TCP:
            json_object_set_new(json_record, "source_port", json_integer(p->sp));
            json_object_set_new(json_record, "destination_port", json_integer(p->dp));
            break;
        default:
            break;
        };

        json_object_set_new(json_record, "source_ip", json_string((char *)inet_ntoa(GET_SRC_ADDR(p))));
        json_object_set_new(json_record, "destination_ip", json_string((char *)inet_ntoa(GET_DST_ADDR(p))));

        json_object_set_new(json_record, "ttl", json_integer(GET_IPH_TTL(p)));
        json_object_set_new(json_record, "tos", json_integer(GET_IPH_TOS(p)));

        json_object_set_new(json_record, "id", json_integer(IS_IP6(p) ? ntohl(GET_IPH_ID(p))
                                                                      : ntohs((uint16_t)GET_IPH_ID(p))));
        json_object_set_new(json_record, "iplen", json_integer(GET_IPH_LEN(p) << 2));

        json_object_set_new(json_record, "dgmlen", json_integer(ntohs(GET_IPH_LEN(p))));
    }

    /* Ethernet */
    if (p->eh != NULL)
    {
        snprintf(construct_buf, BUF_LEN, "%02X:%02X:%02X:%02X:%02X:%02X", p->eh->ether_src[0],
                 p->eh->ether_src[1], p->eh->ether_src[2], p->eh->ether_src[3],
                 p->eh->ether_src[4], p->eh->ether_src[5]);
        json_object_set_new(json_record, "ethsrc", json_string((char *)construct_buf));

        snprintf(construct_buf, BUF_LEN, "%02X:%02X:%02X:%02X:%02X:%02X", p->eh->ether_dst[0],
                 p->eh->ether_dst[1], p->eh->ether_dst[2], p->eh->ether_dst[3],
                 p->eh->ether_dst[4], p->eh->ether_dst[5]);
        json_object_set_new(json_record, "ethdst", json_string((char *)construct_buf));

        snprintf(construct_buf, BUF_LEN, "0x%X", ntohs(p->eh->ether_type));
        json_object_set_new(json_record, "ethtype", json_string((char *)construct_buf));

        snprintf(construct_buf, BUF_LEN, "0x%X", p->pkth->pktlen);
        json_object_set_new(json_record, "ethlen", json_string((char *)construct_buf));
    }

    /* TCP */
    if (p->tcph != NULL)
    {
        snprintf(construct_buf, BUF_LEN, "0x%X", p->tcph->th_seq);
        json_object_set_new(json_record, "tcpseq", json_string((char *)construct_buf));

        snprintf(construct_buf, BUF_LEN, "0x%X", p->tcph->th_ack);
        json_object_set_new(json_record, "tcpack", json_string((char *)construct_buf));

        snprintf(construct_buf, BUF_LEN, "0x%lX", (u_long)ntohl(p->tcph->th_win));
        json_object_set_new(json_record, "tcpwin", json_string((char *)construct_buf));

        json_object_set_new(json_record, "tcplen", json_integer(TCP_OFFSET(p->tcph) << 2));

        char tcpflags[9];
        CreateTCPFlagString(p, tcpflags);
        json_object_set_new(json_record, "tcpflags", json_string((char *)tcpflags));
    }

    /* UDP */
    if (p->udph != NULL)
    {
        json_object_set_new(json_record, "udplength", json_integer(ntohs(p->udph->uh_len)));
    }

    /* ICMP */
    if (p->icmph != NULL)
    {
        json_object_set_new(json_record, "icmptype", json_integer(p->icmph->type));
        json_object_set_new(json_record, "icmpcode", json_integer(p->icmph->code));
        json_object_set_new(json_record, "icmpid", json_integer(ntohs(p->icmph->s_icmp_id)));
        json_object_set_new(json_record, "icmpseq", json_integer(ntohs(p->icmph->s_icmp_seq)));
    }

#ifndef NO_NON_ETHER_DECODER

    if (p->trh != NULL)
    {
        snprintf(construct_buf, BUF_LEN, "%X:%X:%X:%X:%X:%X", p->trh->saddr[0],
                 p->trh->saddr[1], p->trh->saddr[2], p->trh->saddr[3],
                 p->trh->saddr[4], p->trh->saddr[5]);
        json_object_set_new(json_record, "tr_src", json_string((char *)construct_buf));

        snprintf(construct_buf, BUF_LEN, "%X:%X:%X:%X:%X:%X", p->trh->daddr[0],
                 p->trh->daddr[1], p->trh->daddr[2], p->trh->daddr[3],
                 p->trh->daddr[4], p->trh->daddr[5]);
        json_object_set_new(json_record, "tr_dst", json_string((char *)construct_buf));

        snprintf(construct_buf, BUF_LEN, "0x%X", p->trh->ac);
        json_object_set_new(json_record, "tr_ac", json_string((char *)construct_buf));

        snprintf(construct_buf, BUF_LEN, "0x%X", p->trh->fc);
        json_object_set_new(json_record, "tr_fc", json_string((char *)construct_buf));

        if (!p->trhllc)
        {

            snprintf(construct_buf, BUF_LEN, "0x%X", p->trhllc->dsap);
            json_object_set_new(json_record, "tr_dsap", json_string((char *)construct_buf));

            snprintf(construct_buf, BUF_LEN, "0x%X", p->trhllc->ssap);
            json_object_set_new(json_record, "tr_ssap", json_string((char *)construct_buf));

            snprintf(construct_buf, BUF_LEN, "%X%X%X", p->trhllc->protid[0],
                     p->trhllc->protid[1], p->trhllc->protid[2]);
            json_object_set_new(json_record, "tr_protid", json_string((char *)construct_buf));

            snprintf(construct_buf, BUF_LEN, "%X", p->trhllc->ethertype);
            json_object_set_new(json_record, "tr_ethtype", json_string((char *)construct_buf));

            if (p->trhmr)
            {
                snprintf(construct_buf, BUF_LEN, "0x%X", TRH_MR_BCAST(p->trhmr));
                json_object_set_new(json_record, "tr_rif_bcast", json_string((char *)construct_buf));

                snprintf(construct_buf, BUF_LEN, "0x%X", TRH_MR_LEN(p->trhmr));
                json_object_set_new(json_record, "tr_rif_len", json_string((char *)construct_buf));

                snprintf(construct_buf, BUF_LEN, "0x%X", TRH_MR_BCAST(p->trhmr));
                json_object_set_new(json_record, "tr_rif_direction", json_string((char *)construct_buf));

                snprintf(construct_buf, BUF_LEN, "0x%X", TRH_MR_LF(p->trhmr));
                json_object_set_new(json_record, "tr_rif_frsize", json_string((char *)construct_buf));

                snprintf(construct_buf, BUF_LEN, "0x%X", TRH_MR_RES(p->trhmr));
                json_object_set_new(json_record, "tr_rif_res", json_string((char *)construct_buf));

                sprintf(construct_buf, BUF_LEN, "%X:%X:%X:%X:%X:%X:%X:%X",
                        p->trhmr->rseg[0], p->trhmr->rseg[1], p->trhmr->rseg[2],
                        p->trhmr->rseg[3], p->trhmr->rseg[4], p->trhmr->rseg[5],
                        p->trhmr->rseg[6], p->trhmr->rseg[7]);

                json_object_set_new(json_record, "tr_rseg", json_string((char *)construct_buf));
            }
        }
    }

#endif

    //HPFeedsPublish(json_record, config);

    json_decref(json_record);
}

/* == Reused function for hpfeeds ==
 *
 * Functions: HPFeedsReadMsg
 *            HPFeedsGetError
 *            HPFeedsCloseConnection
 *            HPFeedsConnect
 *            HPFeedsPublish
 */

u_char *HPFeedsReadMsg(int sock)
{
    u_char *buffer;
    u_int msglen;

    int len;
    int templen;
    char tempbuf[READ_BLOCK_SIZE];

    if (read(sock, &msglen, 4) != 4)
        FatalError("log_hpfeeds: Fatal read()\n");

    if ((buffer = malloc(ntohl(msglen))) == NULL)
        FatalError("log_hpfeeds: Fatal malloc()\n");

    *(unsigned int *)buffer = msglen;
    msglen = ntohl(msglen);

    len = 4;
    templen = len;
    while ((templen > 0) && (len < msglen))
    {
        templen = read(sock, tempbuf, READ_BLOCK_SIZE);
        memcpy(buffer + len, tempbuf, templen);
        len += templen;
    }

    if (len != msglen)
        FatalError("log_hpfeeds: Fatal read()\n");

    return buffer;
}

void HPFeedsGetError(hpf_msg_t *msg)
{

    u_char *errmsg;

    if (msg)
    {
        if ((errmsg = calloc(1, msg->hdr.msglen - sizeof(msg->hdr))) == NULL)
            FatalError("log_hpfeeds: Fatal write()\n");

        memcpy(errmsg, msg->data, ntohl(msg->hdr.msglen) - sizeof(msg->hdr));

        LogMessage("log_hpfeeds: server error: '%s'\n", errmsg);

        free(errmsg);
        free(msg);
    }
}

void HPFeedsCloseConnection(int *sock)
{
    if (*sock != -1)
    {
        close(*sock);
        *sock = -1;
    }
}

void HPFeedsConnect(HPFeedsConfig *config, int reconnect)
{
    /* socket already on - returning */
    if (config->sock != -1)
        return;

    hpf_msg_t *msg = NULL;
    hpf_chunk_t *chunk;

    unsigned int nonce = 0;

    struct hostent *he;
    //struct sockaddr_in host;

    memset(&host, 0, sizeof(struct sockaddr_in));
    host.sin_family = AF_INET;
    host.sin_port = htons(config->hpfeeds_port);

    if ((he = gethostbyname((char *)config->hpfeeds_host)) == NULL)
        FatalError("log_hpfeeds: Fatal gethostbyname()\n");

    host.sin_addr = *(struct in_addr *)he->h_addr;

    if ((config->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1)
        FatalError("log_hpfeeds: Fatal socket()\n");

    if (connect(config->sock, (struct sockaddr *)&host, sizeof(host)) == -1)
        FatalError("log_hpfeeds: Fatal connect()\n");

    /* Set poll fd */
    config->pfd.fd = config->sock;
    config->pfd.events = POLLIN;
    config->pfd.revents = 0;

    /* Set connection keep alive */
    int optval = 1;

    if (setsockopt(config->sock, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval)) < 0)
    {
        HPFeedsCloseConnection(&config->sock);
        FatalError("log_hpfeeds: Fatal setsockopt()\n");
        return;
    }

    hpfeeds_session_state_t hpfeeds_state = S_INIT;

    for (;;)
    {

        switch (hpfeeds_state)
        {

        case S_INIT:

            if ((msg = (hpf_msg_t *)HPFeedsReadMsg(config->sock)) == NULL)
            {
                HPFeedsCloseConnection(&config->sock);
                break;
            }

            switch (msg->hdr.opcode)
            {

            case OP_INFO:

                chunk = hpf_msg_get_chunk((u_char *)msg + sizeof(msg->hdr), ntohl(msg->hdr.msglen) - sizeof(msg->hdr));

                if (!chunk)
                {
                    LogMessage("log_hpfeeds: invalid message format\n");
                    hpfeeds_state = S_TERMINATE;
                    break;
                }

                nonce = *(unsigned int *)((u_char *)msg + sizeof(msg->hdr) + chunk->len + 1);
                hpfeeds_state = S_AUTH;

                hpf_msg_delete(msg);
                break;

            case OP_ERROR:
                hpfeeds_state = S_ERROR;
                break;

            default:
                hpf_msg_delete(msg);
                hpfeeds_state = S_TERMINATE;
                LogMessage("log_hpfeeds: unknown server message (type %u)\n", msg->hdr.opcode);
                break;
            }

        case S_AUTH:

            LogMessage("log_hpfeeds: sending authentication.\n");

            msg = hpf_msg_auth(nonce, (u_char *)config->hpfeeds_ident, strlen(config->hpfeeds_ident), (u_char *)config->hpfeeds_secret, strlen(config->hpfeeds_secret));

            if (write(config->sock, (u_char *)msg, ntohl(msg->hdr.msglen)) == -1)
                FatalError("log_hpfeeds: Fatal write()");

            if (config->reconnect == 0)
            {
                int rv = poll(&config->pfd, 1, 1000);

                if (rv > 0 && config->pfd.revents && POLLIN)
                {
                    hpfeeds_state = S_ERROR;

                    msg = (hpf_msg_t *)HPFeedsReadMsg(config->sock);
                    break;
                }
            }

            hpfeeds_state = S_AUTH_DONE;
            config->status = HPFEEDS_AUTH_DONE;
            LogMessage("log_hpfeeds: authentication done.\n");
            hpf_msg_delete(msg);

            break;

        case S_ERROR:

            if (msg)
                HPFeedsGetError(msg);

            hpfeeds_state = S_TERMINATE;
            break;

        case S_TERMINATE:
        default:
            HPFeedsCloseConnection(&config->sock);
            LogMessage("log_hpfeeds: connection terminated...\n");
            break;
        }

        if (hpfeeds_state == S_AUTH_DONE || config->sock == -1)
            break;
    }
}

void HPFeedsPublish(json_t *json, HPFeedsConfig *config)
{

    char *data = json_dumps(json, 0);
    unsigned int len = strlen(data);

    hpf_msg_t *msg;

    msg = hpf_msg_publish((u_char *)config->hpfeeds_ident, strlen(config->hpfeeds_ident), (u_char *)config->hpfeeds_channel, strlen(config->hpfeeds_channel), (u_char *)data, len);

    if (write(config->sock, (char *)msg, ntohl(msg->hdr.msglen)) == -1)
    {
        HPFeedsCloseConnection(&config->sock);

        free(data);
        hpf_msg_delete(msg);

        if (config->reconnect)
        {
            HPFeedsConnect(config, config->reconnect);
            HPFeedsPublish(json, config);
            return;
        }

        FatalError("log_hpfeeds: Fatal write()\n");
    }

    /* Do another socket poll - in case of wrong channel */
    if (config->status != HPFEEDS_READY)
    {
        int rv = poll(&config->pfd, 1, 1000);

        if (rv == 0)
        {
            config->status = HPFEEDS_READY;
            LogMessage("log_hpfeeds: Initial publish done.\n");
        }
        else if (rv > 0 && config->pfd.revents && POLLIN)
        {

            config->status = HPFEEDS_NOK;
            hpf_msg_t *error_msg = NULL;

            if ((error_msg = (hpf_msg_t *)HPFeedsReadMsg(config->sock)) != NULL)
            {

                HPFeedsGetError(error_msg);
                LogMessage("log_hpfeeds: Failed to publish.\n");
                HPFeedsCloseConnection(&config->sock);
            }
            else
            {
                FatalError("log_hpfeeds: Something went wrong\n");
            }
        }
    }

    free(data);
    hpf_msg_delete(msg);
}

#endif /* end of !WIN32 */
