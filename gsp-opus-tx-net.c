#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <stdint.h>
#include <opus/opus.h>
#include <ogg/ogg.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <time.h>
#include <stdio.h>
#include <pthread.h>

#include "timef.h"
#include "log.h"
#include "portaudio.h"
#include "control.h"

#define MC_PORT 49876 //audio RX port
#define APP_CONTROL_PORT MC_PORT+2 //TX and RX send packets to this port to respond to APP Control
#define MC_GROUP "172.23.1.120"

#define SAMPLE_RATE 48000
#define ENCODED_BUFFER_SIZE 16384
#define FRAMES_PER_BUFFER 480
#define TXBUFFER_SIZE 16384
#define NUM_CHANNELS 2
#define PA_SAMPLE_TYPE paFloat32
#define PA_DEVICE 2
#define OGG_PAGESIZE 2
#define OPUS_BITRATE 128000
#define OPUS_PACKETLOSS 10

struct sigaction sa;

void cleanup();
void printOptions();

//OpusEncoder *oe;
struct txDest * txDestList=NULL;

//ogg_stream_state os;
ogg_packet op;
ogg_page og;
ogg_int64_t packetno=0;
ogg_int64_t gpos=0;

uint8_t oerr;
uint8_t pagesize=0;

typedef void sigfunc(int);

int arg_pagesize=OGG_PAGESIZE;
int arg_device=PA_DEVICE;
int arg_bitrate=OPUS_BITRATE;
int arg_packetloss=OPUS_PACKETLOSS;
int arg_verbose=0;
int arg_rtp=0;
int arg_port=MC_PORT;
int arg_streamSerial=12345;
int c;

unsigned char * encodedBuffer=0;
unsigned char * txBuffer=0;

//struct sockaddr_in addr;
//int addrlen, sock, cnt;
struct ip_mreq mreq;

PaStream *stream;
pthread_t control_thread_id;


void sig_handler(int signo)
{
	log_info("Terminating...");
	cleanup();
	system ("/bin/stty cooked");
	exit(0);
}

typedef struct
{
	float sLeft;
	float sRight;
}   
paSample;

typedef int PaStreamCallback( const void *input,
		void *output,
		unsigned long frameCount,
		const PaStreamCallbackTimeInfo* timeInfo,
		PaStreamCallbackFlags statusFlags,
		void *userData ) ;


int getAudioDevicesCount(int * nDevices) {
	int numDevices;
	PaError err;
	numDevices = Pa_GetDeviceCount();
	if( numDevices < 0 )
	{
		log_error( "ERROR: Pa_CountDevices returned 0x%x", numDevices );
		err = numDevices;
		return err;
	} else {
		(*nDevices)=numDevices;
		return 0;
	}
}

struct sockaddr_in app_addr;
int app_addrlen, app_sock;
struct in_addr app_in_addr;

void setupAppControlSocket() {
	/* set up socket */
	app_sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (app_sock < 0) {
		log_error("failed to create app_socket");
		exit(1);
	}
	bzero((char *)&app_addr, sizeof(app_addr));
	app_addr.sin_family = AF_INET;
	app_addr.sin_port = htons(APP_CONTROL_PORT);
	app_addrlen = sizeof(app_addr);
}

void control_setupStreamingSocket(int * sock, struct sockaddr_in * addr, int * addrlen, struct in_addr * sender_dst) {
	/* set up socket */
	*sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (*sock < 0) {
		log_error("socket setup failure (setupSocket)");
		exit(1);
	}
	bzero((char *)addr, sizeof(*addr));
	addr->sin_family = AF_INET;
	addr->sin_addr = *sender_dst;
	addr->sin_port = htons(MC_PORT);
	*addrlen = sizeof(*addr);
}

void  printAudioDevices() {
	int nDevices=0;
	const PaDeviceInfo *device;
	getAudioDevicesCount(&nDevices);
	for (int i=0;i<nDevices;i++) {
		device=Pa_GetDeviceInfo(i);
		log_info("Device No: %i:%s",i,device->name);
	}

}



uint64_t ssm=0;
uint64_t ssmSamples=0;
uint8_t paCallbackRunning=0;

static int paCallback( const void *inputBuffer, void *outputBuffer,
		unsigned long framesPerBuffer,
		const PaStreamCallbackTimeInfo* timeInfo,
		PaStreamCallbackFlags statusFlags,
		void *userData )
{
	unsigned int i;
	opus_int32 oBufSize;
	struct timespec tStart;
	struct tm tmStart;
	uint64_t callbackTime;

	// populate the start time 
	//
	// wait for a second boundary.

	if (!paCallbackRunning) {
		if (ssm==0) {

			/*
			 * first time around we grab Seconds Since Midnight
			 * and populate ssm
			 *
			 * Next time around we check to see if we have passed
			 * a second boundary.
			 */
			clock_gettime(CLOCK_REALTIME,&tStart);
			localtime_r(&tStart.tv_sec,&tmStart);
			ssm=getSecondsSinceMidnight(&tmStart);
			return 0;
		} else {
			/*
			 * second time around the callback - 
			 * have we passed a boundary?
			 */
			clock_gettime(CLOCK_REALTIME,&tStart);
			localtime_r(&tStart.tv_sec,&tmStart);
			callbackTime=getSecondsSinceMidnight(&tmStart);
			if (callbackTime>ssm) {
				paCallbackRunning=1;
				ssm=callbackTime;
				ssmSamples=callbackTime*SAMPLE_RATE;
			} else
				return 0;
		}
	}

	if (paCallbackRunning) {

		ssmSamples+=framesPerBuffer;

		struct txDest * txDestList_iterator=txDestList;

		while (txDestList_iterator != NULL) {

			memset(txDestList_iterator->encodedBuffer,0,ENCODED_BUFFER_SIZE);
			memset(txDestList_iterator->txBuffer,0,TXBUFFER_SIZE);
			memset(&op,0,sizeof(ogg_packet));

			oBufSize=opus_encode_float(txDestList_iterator->oe,(const float *)inputBuffer,framesPerBuffer,txDestList_iterator->encodedBuffer,ENCODED_BUFFER_SIZE);

			op.packet=txDestList_iterator->encodedBuffer;
			op.bytes=oBufSize;
			op.b_o_s=(packetno==0?1:0);
			op.e_o_s=0;
			op.granulepos=ssmSamples;
			op.packetno=packetno++;

			ogg_stream_packetin(&txDestList_iterator->os,&op);
			txDestList_iterator->pagesize = ((txDestList_iterator->pagesize+1) % arg_pagesize);

			if (txDestList_iterator->pagesize == 0 && ogg_stream_flush(&txDestList_iterator->os,&og)) {
				memcpy(txDestList_iterator->txBuffer,og.header,og.header_len);
				memcpy(txDestList_iterator->txBuffer+og.header_len,og.body,og.body_len);
				sendto(txDestList_iterator->sock,txDestList_iterator->txBuffer,og.header_len+og.body_len,0,(struct sockaddr *) &txDestList_iterator->addr,txDestList_iterator->addrlen);
			}

			txDestList_iterator=txDestList_iterator->next;
		}


		//gpos+=framesPerBuffer;


	}

	return 0;
}

void displayHelp() {
	printf("GSP Opus Streamer 0.1 2018\n\n");
	printf("-h 		Show Help\n");
	printf("-v		Verbose\n");
	printf("-l		List Devices\n");
	printf("-e <serial>	Stream Serial Number\n");
	printf("-d <device>	Device ID\n");
}

int main(int argc,char *argv[]) {

	sa.sa_handler=sig_handler;
	sigaction(SIGINT,&sa,NULL);

	PaError err;
	static paSample data;
	PaStreamParameters inputParameters;
	PaStreamParameters outputParameters;
	int oeErr;
	int opt;

	//inet_aton(MC_GROUP,&arg_dst); // destination IP. we use strncpy to avoid buffer overrun.

	while ((opt = getopt(argc, argv, "hld:e:")) != -1) {
		switch (opt) {
			case 'e': //Stream Serial number 
				arg_streamSerial = atoi(optarg);
				break;
			case 'd': //device ID
				arg_device = atoi(optarg);
				break;
			case 'l': //Audio Devices
				Pa_Initialize();
				printAudioDevices();
				Pa_Terminate();
				exit(0);
				break;
			case 'h': //help
				displayHelp();
				exit(0);
				break;
			default:
				break;
		}
	}

	Pa_Initialize();

	printOptions();

	encodedBuffer=(unsigned char *)calloc(sizeof(unsigned char),ENCODED_BUFFER_SIZE);
	txBuffer=(unsigned char *)calloc(sizeof(unsigned char),TXBUFFER_SIZE);

	setupAppControlSocket();


	log_info("App Control Socket Available");
	memset(&inputParameters,0,sizeof(PaStreamParameters));
	memset(&outputParameters,0,sizeof(PaStreamParameters));
	log_info("Pa Params Cleared");

	/* -- setup input and output -- */
	inputParameters.device = arg_device;
	inputParameters.channelCount = NUM_CHANNELS;
	inputParameters.sampleFormat = PA_SAMPLE_TYPE;
	inputParameters.suggestedLatency = Pa_GetDeviceInfo( inputParameters.device )->defaultLowInputLatency ;
	inputParameters.hostApiSpecificStreamInfo = NULL;
	outputParameters.device = arg_device;
	outputParameters.channelCount = NUM_CHANNELS;
	outputParameters.sampleFormat = PA_SAMPLE_TYPE;
	outputParameters.suggestedLatency = Pa_GetDeviceInfo( outputParameters.device )->defaultLowInputLatency ;
	outputParameters.hostApiSpecificStreamInfo = NULL;
	
	log_info("Pa Params Populated");
	/* -- setup stream -- */
	err = Pa_OpenStream(
			&stream,
			&inputParameters,
			NULL,
			SAMPLE_RATE,
			FRAMES_PER_BUFFER,
			paClipOff,      /* we won't output out of range samples so don't bother clipping them */
			paCallback, /* no callback, use blocking API */
			NULL ); /* no callback, so no callback userData */

	log_debug("Pa_OpenStream error %u",err);

	if( err != paNoError ) goto error;
	/* Open an audio I/O stream. */

	log_debug ("Audio Stream Open");

	pthread_create(&control_thread_id,NULL,&control_thread_function,NULL) ;

	log_info("Preparing to start Stream...");
	err = Pa_StartStream(stream);
	if (err != paNoError) goto error;

	while (1) {
		c=getchar();
		switch (c) {
			case 'q':
				cleanup();
				exit(0);
				break;
			case 'o':
				printOptions();
				break;
		}
	}

error:

	exit(0);

}

void cleanup() {
	//ogg_stream_destroy(&os);
	//opus_encoder_destroy(oe);
	Pa_CloseStream(stream);
	Pa_Terminate();
	system("/bin/stty cooked");
}

void printOptions() {
	log_info("Device Name:		%s",Pa_GetDeviceInfo(arg_device)->name) ;
	log_info("Stream Serial:	%u",arg_streamSerial);
}
