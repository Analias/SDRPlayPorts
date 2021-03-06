/*
 * rtl-sdr, turns your Realtek RTL2832 based DVB dongle into a SDR receiver
 * Copyright (C) 2012 by Steve Markgraf <steve@steve-m.de>
 * Copyright (C) 2012-2013 by Hoernchen <la@tfc-server.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ************************************** THIS IS A FORK ******************* ORIGINAL COPYRIGHT SEE ABOVE.
 *
 *  SDRPlayPorts
 *  Ports of some parts of rtl-sdr for the SDRPlay (original: git://git.osmocom.org/rtl-sdr.git /)
 *  2016: Fork by HB9FXQ (Frank Werner-Krippendorf, mail@hb9fxq.ch)
 *
 *  Code changes I've done:
 *  - removed rtl_sdr related calls and replaced them with mir_* to work with the SDRPlay
 *  - removed various local variables
 *
 *  This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 *
 */

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef _WIN32
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <fcntl.h>
#else
#include <winsock2.h>
#include "getopt/getopt.h"
#endif

#include <pthread.h>

#include "mirsdrapi-rsp.h"

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")

typedef int socklen_t;

#else
#define closesocket close
#define SOCKADDR struct sockaddr
#define DEFAULT_BUF_LENGTH		(336 * 2) // (16 * 16384)
#define SOCKET int
#define SOCKET_ERROR -1
#define DEFAULT_SAMPLE_RATE		2048000//FIXME


#endif

static SOCKET s;

static pthread_t tcp_worker_thread;
static pthread_t command_thread;
static pthread_cond_t exit_cond;

static pthread_mutex_t exit_cond_lock;
static pthread_mutex_t ll_mutex;

static pthread_cond_t cond;
static uint32_t cmd_freq_value;

int samplesPerPacket, grChanged, fsChanged, rfChanged;

static uint32_t bytes_to_read = 0;

struct llist {
    char *data;
    size_t len;
    struct llist *next;
};

typedef struct { /* structure size must be multiple of 2 bytes */
    char magic[4];
    uint32_t tuner_type;
    uint32_t tuner_gain_count;
} dongle_info_t;


typedef struct{
    uint32_t allocfrom;
    uint32_t allocto;
} frequency_allocation_t;

/*
 * frequency allocation table, see chapter 6 in mirics API spec
 *
 * .... . If a frequency is
 * desired that falls outside the current band then a mir_sdr_Uninit command must be issued followed by a
 * mir_sdr_Init command at the new frequency to force reconfiguration of the front end.....
 *
 */
frequency_allocation_t freqAllocationTable[8] = {{0, 11.999999e6}
                                                ,{12e6,  29.999999e6}
                                                ,{30e6,  59.999999e6}
                                                ,{60e6,  119.999999e6}
                                                ,{120e6, 249.999999e6}
                                                ,{250e6, 419.999999e6}
                                                ,{420e6, 999.999999e6}
                                                ,{1000e6,UINT32_MAX}};

static struct llist *ll_buffers = 0;
static int llbuf_num = 500;

uint32_t out_block_size = DEFAULT_BUF_LENGTH;
short *ibuf;
short *qbuf;
unsigned int firstSample;
int n_read;
int sdrIsInitialized = 0; /* 1, when mir_sdr_init done */
mir_sdr_ErrT r;
uint32_t frequency = 100000000;
int gain = 30;
uint32_t samp_rate = DEFAULT_SAMPLE_RATE;
uint8_t *buffer;
mir_sdr_Bw_MHzT sdr_bw = mir_sdr_BW_1_536;
int rspMode = 0;
int rspLNA = 0;

static volatile int do_exit = 0;

void usage(void)
{
    printf("play_tcp (rtl_tcp fork for SDRPlay), an I/Q spectrum server for SDRPlay receivers\n\n"
                   "Usage:\t[-a listen address]\n"
                   "\t[-p listen port (default: 1234)]\n"
                   "\t[-f frequency to tune to [Hz]]\n"
                   "\t[-g SDRPlay Gain reduction], see http://www.sdrplay.com/docs/Mirics_SDR_API_Specification.pdf for details\n"
                   "\t[-s samplerate in Hz (default: 2048000 Hz)]\n"
                   "\t[-b number of buffers (default: 15, set by library)]\n"
                   "\t[-n max number of linked list buffers to keep (default: 500)]\n"
                   "\t[-r enable gain reduction (default: 0, disabled)]\n"
                   "\t[-l RSP LNA enable (default: 0, disabled)]\n");
    exit(1);
}

#ifdef _WIN32
int gettimeofday(struct timeval *tv, void* ignored)
{
	FILETIME ft;
	unsigned __int64 tmp = 0;
	if (NULL != tv) {
		GetSystemTimeAsFileTime(&ft);
		tmp |= ft.dwHighDateTime;
		tmp <<= 32;
		tmp |= ft.dwLowDateTime;
		tmp /= 10;
#ifdef _MSC_VER
		tmp -= 11644473600000000Ui64;
#else
		tmp -= 11644473600000000ULL;
#endif
		tv->tv_sec = (long)(tmp / 1000000UL);
		tv->tv_usec = (long)(tmp % 1000000UL);
	}
	return 0;
}

BOOL WINAPI
sighandler(int signum)
{
	if (CTRL_C_EVENT == signum) {
		fprintf(stderr, "Signal caught, exiting!\n");
		do_exit = 1;
		rtlsdr_cancel_async(dev);
		return TRUE;
	}
	return FALSE;
}
#else
static void sighandler(int signum)
{
    fprintf(stderr, "Signal caught, exiting!\n");
    // TODO: replace rtlsdr_cancel_async(dev);
    do_exit = 1;
}
#endif

void rtlsdr_callback(unsigned char *buf, uint32_t len)
{
    if(!do_exit) {
        struct llist *rpt = (struct llist*)malloc(sizeof(struct llist));
        rpt->data = (char*)malloc(len);
        memcpy(rpt->data, buf, len);
        rpt->len = len;
        rpt->next = NULL;

        pthread_mutex_lock(&ll_mutex);

        if (ll_buffers == NULL) {
            ll_buffers = rpt;
        } else {
            struct llist *cur = ll_buffers;
            int num_queued = 0;

            while (cur->next != NULL) {
                cur = cur->next;
                num_queued++;
            }

            if(llbuf_num && llbuf_num == num_queued-2){
                struct llist *curelem;

                free(ll_buffers->data);
                curelem = ll_buffers->next;
                free(ll_buffers);
                ll_buffers = curelem;
            }

            cur->next = rpt;

        }
        pthread_cond_signal(&cond);
        pthread_mutex_unlock(&ll_mutex);
    }
}

static void *tcp_worker(void *arg)
{
    struct llist *curelem,*prev;
    int bytesleft,bytessent, index;
    struct timeval tv= {1,0};
    struct timespec ts;
    struct timeval tp;
    fd_set writefds;
    int r = 0;

    while(1) {
        if(do_exit)
            pthread_exit(0);

        pthread_mutex_lock(&ll_mutex);
        gettimeofday(&tp, NULL);
        ts.tv_sec  = tp.tv_sec+5;
        ts.tv_nsec = tp.tv_usec * 1000;
        r = pthread_cond_timedwait(&cond, &ll_mutex, &ts);
        if(r == ETIMEDOUT) {
            pthread_mutex_unlock(&ll_mutex);
            printf("worker cond timeout\n");
            sighandler(0);
            pthread_exit(NULL);
        }

        curelem = ll_buffers;
        ll_buffers = 0;
        pthread_mutex_unlock(&ll_mutex);

        while(curelem != 0) {
            bytesleft = curelem->len;
            index = 0;
            bytessent = 0;
            while(bytesleft > 0) {
                FD_ZERO(&writefds);
                FD_SET(s, &writefds);
                tv.tv_sec = 1;
                tv.tv_usec = 0;
                r = select(s+1, NULL, &writefds, NULL, &tv);
                if(r) {
                    bytessent = send(s,  &curelem->data[index], bytesleft, 0);
                    bytesleft -= bytessent;
                    index += bytessent;
                }
                if(bytessent == SOCKET_ERROR || do_exit) {
                    printf("worker socket bye\n");
                    sighandler(0);
                    pthread_exit(NULL);
                }
            }
            prev = curelem;
            curelem = curelem->next;
            free(prev->data);
            free(prev);
        }
    }
}

#ifdef _WIN32
#define __attribute__(x)
#pragma pack(push, 1)
#endif
struct command{
    unsigned char cmd;
    unsigned int param;
}__attribute__((packed));

#ifdef _WIN32
#pragma pack(pop)
#endif
static void *command_worker(void *arg)
{
    int left, received = 0;
    fd_set readfds;
    struct command cmd={0, 0};
    struct timeval tv= {1, 0};
    int r = 0;
    uint32_t tmp;

    while(1) {
        left=sizeof(cmd);
        while(left >0) {
            FD_ZERO(&readfds);
            FD_SET(s, &readfds);
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            r = select(s+1, &readfds, NULL, NULL, &tv);
            if(r) {
                received = recv(s, (char*)&cmd+(sizeof(cmd)-left), left, 0);
                left -= received;
            }
            if(received == SOCKET_ERROR || do_exit) {
                printf("comm recv bye\n");
                sighandler(0);
                pthread_exit(NULL);
            }
        }
        switch(cmd.cmd) {
            case 0x01:
                printf("set freq %d\n", ntohl(cmd.param));
                cmd_freq_value = ntohl(cmd.param);
                break;
            case 0x02:
                printf("set sample rate %d\n !Not implemented for SDRPlay (not yet...)\n", ntohl(cmd.param));
                break;
            case 0x03:
                printf("set gain mode %d\n !Not implemented for SDRPlay (not yet...)\n", ntohl(cmd.param));
                break;
            case 0x04:
                printf("set gain %d\n !Not implemented for SDRPlay (not yet...)\n", ntohl(cmd.param));
                printf("SDRPlay gain update currently ignored, set at startup with 'rtl_tcp -g'\n");
                break;
            case 0x05:
                printf("set freq correction %d\n !Not implemented for SDRPlay (not yet...)\n", ntohl(cmd.param));
                break;
            case 0x06:
                tmp = ntohl(cmd.param);
                printf("set if stage %d gain %d\n", tmp >> 16, (short)(tmp & 0xffff));               
                break;
            case 0x07:
                printf("set test mode %d\n !Not implemented for SDRPlay (not yet...)\n", ntohl(cmd.param));
                break;
            case 0x08:
                printf("set agc mode %d\n !Not implemented for SDRPlay (not yet...)\n", ntohl(cmd.param));
                break;
            case 0x09:
                printf("set direct sampling %d\n !Not implemented for SDRPlay (not yet...)\n", ntohl(cmd.param));
                break;
            case 0x0a:
                printf("set offset tuning %d\n !Not implemented for SDRPlay (not yet...) (not yet...)\n", ntohl(cmd.param));
                break;
            case 0x0b:
                printf("set rtl xtal %d\n !Not implemented for SDRPlay (not yet...) (not yet...)\n", ntohl(cmd.param));
                break;
            case 0x0c:
                printf("set tuner xtal %d\n !Not implemented for SDRPlay (not yet...)\"", ntohl(cmd.param));
                break;
            case 0x0d:
                printf("set tuner gain by index %d\n !Not implemented for SDRPlay (not yet...)\"", ntohl(cmd.param));
                break;
            default:
                break;
        }
        cmd.cmd = 0xff;
    }
}



void sdrplay_reinit(){

    printf("======>>>>> REINIT F: %d\n", frequency);

    if(sdrIsInitialized == 1) {
        r = mir_sdr_Uninit();
    }

    if (rspMode == 1)
    {
        mir_sdr_SetParam(201,1);
        if (rspLNA == 1)
        {
            mir_sdr_SetParam(202,0);
        }
        else
        {
            mir_sdr_SetParam(202,1);
        }
        r = mir_sdr_Init(gain, (samp_rate/1e6), (frequency/1e6),
                         sdr_bw, mir_sdr_IF_Zero, &samplesPerPacket );
    }
    else
    {
        r = mir_sdr_Init((78-gain), (samp_rate/1e6), (frequency/1e6),
                         sdr_bw, mir_sdr_IF_Zero, &samplesPerPacket );
    }

    if (r != mir_sdr_Success) {
        fprintf(stderr, "Failed to start SDRplay RSP device.\n");
        exit(1);
    }

    while(mir_sdr_Success != mir_sdr_SetRf(frequency, 1, 0)){
        printf("SetRf rejected, retry....\n");
    }

    printf("SetRf to %d\n", frequency);

    mir_sdr_SetDcMode(4,0);
    mir_sdr_SetDcTrackTime(63);

    if (r != mir_sdr_Success) {
        fprintf(stderr, "Failed to start SDRplay RSP device.\n");
        exit(1);
    }

    // Configure DC tracking in tuner


    if (r != mir_sdr_Success) {
        fprintf(stderr, "Failed to onfigure DC tracking in tuner of RSP device.\n");
    }

    sdrIsInitialized = 1;

}

void sdrplay_rx(){

    buffer = malloc(out_block_size * sizeof(uint8_t));
    sdrplay_reinit();



    ibuf = malloc(samplesPerPacket * sizeof(short));
    qbuf = malloc(samplesPerPacket * sizeof(short));

    int i, j;



    while (!do_exit) {



        if(cmd_freq_value != frequency){

            if(freq_change_req_reinnit(frequency,cmd_freq_value) == 1) {

                frequency = cmd_freq_value;
                sdrplay_reinit();
            }else{
                frequency = cmd_freq_value; // update tracking freq;
                mir_sdr_SetRf(frequency, 1, 0);
            }

            printf("*************** freq change req ****************\n");

        }

        r = mir_sdr_ReadPacket(ibuf, qbuf, &firstSample, &grChanged, &rfChanged,
                               &fsChanged);


        if (r != mir_sdr_Success) {
            fprintf(stderr, "WARNING: ReadPacket failed.\n");
            break;
        }

        j = 0;
        for (i=0; i < samplesPerPacket; i++)
        {
            buffer[j++] = (unsigned char) (ibuf[i] >> 8);
            buffer[j++] = (unsigned char) (qbuf[i] >> 8);
        }

        n_read = (samplesPerPacket * 2);

        if ((bytes_to_read > 0) && (bytes_to_read <= (uint32_t)n_read)) {
            n_read = bytes_to_read;
            do_exit = 1;
        }

        rtlsdr_callback(buffer, n_read);

        if (bytes_to_read > 0)
            bytes_to_read -= n_read;
    }


}

double atofs(char *s)
/* standard suffixes */
{
    char last;
    int len;
    double suff = 1.0;
    len = strlen(s);
    last = s[len - 1];
    s[len - 1] = '\0';
    switch (last) {
        case 'g':
        case 'G':
            suff *= 1e3;
        case 'm':
        case 'M':
            suff *= 1e3;
        case 'k':
        case 'K':
            suff *= 1e3;
            suff *= atof(s);
            s[len - 1] = last;
            return suff;
    }
    s[len - 1] = last;
    return atof(s);

}


int main(int argc, char **argv)
{
    int r, opt, i;
    char* addr = "127.0.0.1";
    int port = 1234;

    struct sockaddr_in local, remote;
    uint32_t buf_num = 0;
    int dev_index = 0;
    int dev_given = 0;
    gain = 30;
    int ppm_error = 0;
    struct llist *curelem,*prev;
    pthread_attr_t attr;
    void *status;
    struct timeval tv = {1,0};
    struct linger ling = {1,0};
    SOCKET listensocket;
    socklen_t rlen;
    fd_set readfds;
    u_long blockmode = 1;
    dongle_info_t dongle_info;

#ifdef _WIN32
    WSADATA wsd;
	i = WSAStartup(MAKEWORD(2,2), &wsd);
#else
    struct sigaction sigact, sigign;
#endif

    while ((opt = getopt(argc, argv, "a:p:f:g:s:b:n:d:P:r:l:")) != -1) {
        switch (opt) {
            case 'd':
                //dev_index = verbose_device_search(optarg);
                //dev_given = 1;//FIXME
                break;
            case 'r':
                rspMode = atoi(optarg);
                break;
            case 'l':
                rspLNA = atoi(optarg);
                break;
            case 'f':
                frequency = (uint32_t)atofs(optarg);
                break;
            case 'g':
                gain = (int)(atof(optarg) * 10); /* tenths of a dB *///FIXME
                break;
            case 's':
                samp_rate = (uint32_t)atofs(optarg);//FIXME
                break;
            case 'a':
                addr = optarg;
                break;
            case 'p':
                port = atoi(optarg);
                break;
            case 'b':
                buf_num = atoi(optarg);
                break;
            case 'n':
                llbuf_num = atoi(optarg);
                break;
            case 'P':
                ppm_error = atoi(optarg);
                break;
            default:
                usage();
                break;
        }
    }

    if (argc < optind)
        usage();

    if (!dev_given) {
        //dev_index = verbose_device_search("0");
    }

    if (dev_index < 0) {
        exit(1);
    }

#ifndef _WIN32
    sigact.sa_handler = sighandler;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = 0;
    sigign.sa_handler = SIG_IGN;
    sigaction(SIGINT, &sigact, NULL);
    sigaction(SIGTERM, &sigact, NULL);
    sigaction(SIGQUIT, &sigact, NULL);
    sigaction(SIGPIPE, &sigign, NULL);
#else
    SetConsoleCtrlHandler( (PHANDLER_ROUTINE) sighandler, TRUE );
#endif

    pthread_mutex_init(&exit_cond_lock, NULL);
    pthread_mutex_init(&ll_mutex, NULL);
    pthread_mutex_init(&exit_cond_lock, NULL);
    pthread_cond_init(&cond, NULL);
    pthread_cond_init(&exit_cond, NULL);

    memset(&local,0,sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = inet_addr(addr);

    listensocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    r = 1;
    setsockopt(listensocket, SOL_SOCKET, SO_REUSEADDR, (char *)&r, sizeof(int));
    setsockopt(listensocket, SOL_SOCKET, SO_LINGER, (char *)&ling, sizeof(ling));
    bind(listensocket,(struct sockaddr *)&local,sizeof(local));

#ifdef _WIN32
    ioctlsocket(listensocket, FIONBIO, &blockmode);
#else
    r = fcntl(listensocket, F_GETFL, 0);
    r = fcntl(listensocket, F_SETFL, r | O_NONBLOCK);
#endif

    while(1) {
        printf("listening...\n");
        printf("Use the device argument 'rtl_tcp=%s:%d' in OsmoSDR "
                       "(gr-osmosdr) source\n"
                       "to receive samples in GRC and control "
                       "rtl_tcp parameters (frequency, gain, ...).\n",
               addr, port);
        listen(listensocket,1);

        while(1) {
            FD_ZERO(&readfds);
            FD_SET(listensocket, &readfds);
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            r = select(listensocket+1, &readfds, NULL, NULL, &tv);
            if(do_exit) {
                goto out;
            } else if(r) {
                rlen = sizeof(remote);
                s = accept(listensocket,(struct sockaddr *)&remote, &rlen);
                break;
            }
        }

        setsockopt(s, SOL_SOCKET, SO_LINGER, (char *)&ling, sizeof(ling));

        printf("client accepted!\n");

        memset(&dongle_info, 0, sizeof(dongle_info));
        memcpy(&dongle_info.magic, "RTL0", 4);

        //r = rtlsdr_get_tuner_type(dev);
        if (r >= 0)
            dongle_info.tuner_type = htonl(r);

        //r = rtlsdr_get_tuner_gains(dev, NULL);
        if (r >= 0)
            dongle_info.tuner_gain_count = htonl(r);

        r = send(s, (const char *)&dongle_info, sizeof(dongle_info), 0);
        if (sizeof(dongle_info) != r)
            printf("failed to send dongle information\n");

        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
        r = pthread_create(&tcp_worker_thread, &attr, tcp_worker, NULL);
        r = pthread_create(&command_thread, &attr, command_worker, NULL);
        pthread_attr_destroy(&attr);

       // r = rtlsdr_read_async(dev, rtlsdr_callback, NULL, buf_num, 0);
        sdrplay_rx();

        pthread_join(tcp_worker_thread, &status);
        pthread_join(command_thread, &status);

        closesocket(s);

        printf("all threads dead..\n");
        curelem = ll_buffers;
        ll_buffers = 0;

        while(curelem != 0) {
            prev = curelem;
            curelem = curelem->next;
            free(prev->data);
            free(prev);
        }

        do_exit = 0;
    }



    out:
    mir_sdr_Uninit();
    closesocket(listensocket);
    closesocket(s);
#ifdef _WIN32
    WSACleanup();
#endif
    printf("bye!\n");
    return r >= 0 ? r : -r;
}



int freq_change_req_reinnit(uint32_t old, uint32_t new){

    int i;

    for(i=0; i<8; i++){

        frequency_allocation_t *curr = &freqAllocationTable[i];

        if(old >= curr->allocfrom && old <= curr->allocto){
            return new > curr->allocto || new < curr->allocfrom;
        }
    }

    return 1;

}
