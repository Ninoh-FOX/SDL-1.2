#include "SDL_config.h"

/* Allow access to a raw mixing buffer */

#include <stdio.h>	/* For perror() */
#include <string.h>	/* For strerror() */
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/stat.h>

#define DEBUG_AUDIO

#include "SDL_timer.h"
#include "SDL_audio.h"
#include "../SDL_audiomem.h"
#include "../SDL_audio_c.h"
#include "../SDL_audiodev_c.h"
#include "SDL_miaoaudio.h"

#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <linux/input.h>
#include <SDL/SDL.h>

#include <msettings.h> // TODO: remove me

/* The tag name used by MI_AO audio */
#define MIAO_DRIVER_NAME         "miao"

/* Audio driver functions */
static int MIAO_OpenAudio(_THIS, SDL_AudioSpec *spec);
static void MIAO_WaitAudio(_THIS);
static void MIAO_PlayAudio(_THIS);
static Uint8* MIAO_GetAudioBuf(_THIS);
static void MIAO_CloseAudio(_THIS);

static int Audio_Available(void)
{
    MI_AUDIO_Attr_t    attr;
    memset(&attr, 0, sizeof(attr));
    attr.eSamplerate = 48000;
    attr.eSoundmode = 1;
    attr.u32ChnCnt = 2;
    attr.u32PtNumPerFrm = 256;
    if (MI_AO_SetPubAttr(0,&attr)) return(0);
    return(1);
}

static void Audio_DeleteDevice(SDL_AudioDevice *device)
{
	SDL_free(device->hidden);
	SDL_free(device);
}

static SDL_AudioDevice *Audio_CreateDevice(int devindex)
{
	fprintf(stdout, "create miao audio device...\n");
	
	SDL_AudioDevice *this;

	/* Initialize all variables that we clean on shutdown */
	this = (SDL_AudioDevice *)SDL_malloc(sizeof(SDL_AudioDevice));
	if ( this ) {
		SDL_memset(this, 0, (sizeof *this));
		this->hidden = (struct SDL_PrivateAudioData *)
				SDL_malloc((sizeof *this->hidden));
	}
	if ( (this == NULL) || (this->hidden == NULL) ) {
		SDL_OutOfMemory();
		if ( this ) {
			SDL_free(this);
		}
		return(0);
	}
	SDL_memset(this->hidden, 0, (sizeof *this->hidden));

	/* Set the function pointers */
	this->OpenAudio = MIAO_OpenAudio;
	this->WaitAudio = MIAO_WaitAudio;
	this->PlayAudio = MIAO_PlayAudio;
	this->GetAudioBuf = MIAO_GetAudioBuf;
	this->CloseAudio = MIAO_CloseAudio;

	this->free = Audio_DeleteDevice;

	fprintf(stdout, "created miao audio device!\n");
	
	return this;
}

AudioBootStrap MIAO_bootstrap = {
	MIAO_DRIVER_NAME, "MI AO audio for Miyoo Mini",
	Audio_Available, Audio_CreateDevice
};

// based on AOSample_rev7

// #define	YIELD_WAIT	// Flag to wait with sched_yield() when the wait time is less than 10.5ms
			// ( callback function will be called at more precise timing,
			//   but may cause blocking/slowdown of low-priority daemons/threads execution )
#ifdef	YIELD_WAIT
#include <sched.h>
#ifndef	SCHED_IDLE
#define SCHED_IDLE	5
#endif
#endif

static struct timeval tod;
static int usleepclock;
static uint64_t startclock;
static uint64_t targetclock;
static uint64_t clock_freqframes;
static uint32_t framecounter;
static uint32_t num_frames;
#ifdef	YIELD_WAIT
static int policy;
static struct sched_param scprm;
#endif

/* This function waits until it is possible to write a full sound buffer */
static void MIAO_WaitAudio(_THIS)
{
	framecounter++;
	if (framecounter == (uint32_t)this->spec.freq) {
		framecounter = 0;
		startclock += clock_freqframes;
	}
	targetclock = framecounter * clock_freqframes / this->spec.freq + startclock;
	gettimeofday(&tod, NULL);
	usleepclock = targetclock - (tod.tv_usec + tod.tv_sec * 1000000);
	// check 300ms under/overrun (1frame max = 256ms at 8kHz/2048samples)
	if ((usleepclock < -300000)||(usleepclock > 300000)) {
		// reset buffer
		MI_AO_ClearChnBuf(0,0);
		memset(mixbuf, 0, mixlen);
		for (uint32_t i=num_frames-1; i>0; i--) MI_AO_SendFrame(0, 0, frame, 0);
		framecounter = 0;
		gettimeofday(&tod, NULL);
		startclock = tod.tv_usec + tod.tv_sec * 1000000;
	} else if (usleepclock > 0) {
#ifdef	YIELD_WAIT
		// wait process for miyoomini with 10ms sleep precision
		if (usleepclock > 10500) usleep(usleepclock - 10500);	// 0.5ms margin
		// wait for less than 10.5ms with sched_yield()
		sched_setscheduler(0, SCHED_IDLE, &scprm);
		do { sched_yield(); gettimeofday(&tod, NULL);
		} while (targetclock > (tod.tv_usec + tod.tv_sec * 1000000));
		sched_setscheduler(0, policy, &scprm);
#else
		usleep(usleepclock);
#endif
	}
}

static void MIAO_PlayAudio(_THIS)
{
	// MI_S32 ret = MI_AO_SendFrame(0, 0, frame, 0);
	// if (ret) fprintf(stdout, "MI_Error: err:0x%x\n", ret);
	
	// NOTE: this is not the cause of the crash/hang
	// or at least the response to it evaluating to true isn't
	if (MI_AO_SendFrame(0, 0, frame, 0))
	{
		perror("Audio write");
		this->enabled = 0;
	}

#ifdef DEBUG_AUDIO
	// fprintf(stdout, "Wrote %d bytes of audio data\n", mixlen);
#endif
}

static Uint8 *MIAO_GetAudioBuf(_THIS)
{
	return(mixbuf);
}

static void MIAO_CloseAudio(_THIS)
{
	fprintf(stdout, "close miao audio\n");
	
	MI_AO_ClearChnBuf(0,0);
	MI_AO_DisableChn(0,0);
	MI_AO_Disable(0);
	
	QuitSettings();
	
	if ( mixbuf != NULL ) {
		SDL_FreeAudioMem(mixbuf);
		mixbuf = NULL;
	}
	
	if ( frame != NULL) {
		SDL_FreeAudioMem(frame);
		frame = NULL;
	}
}

//		signed 16bit only
//		sampling rate: 8/11.025/12/16/22.05/24/32/44.1/48kHz
//		samples should be 2048 or less

static int MIAO_OpenAudio(_THIS, SDL_AudioSpec *spec)
{
	fprintf(stdout, "open miao audio\n");
	
	MI_AUDIO_Attr_t	attr;
	const int freqtable[] = { 8000,11025,12000,16000,22050,24000,32000,44100,48000 };
	uint32_t	i;

	spec->format = AUDIO_S16SYS;
	for (i=0; i<(sizeof(freqtable)/sizeof(int)); i++) {
		if (spec->freq <= freqtable[i]) { spec->freq = freqtable[i]; break; }
	} if (spec->freq > 48000) spec->freq = 48000;
	if (spec->samples > 2048) spec->samples = 2048;
	else if (spec->samples < 8) spec->samples = 8;
	spec->size = spec->samples * spec->channels * 2;

	memset(&attr, 0, sizeof(attr));
	attr.eSamplerate = (MI_AUDIO_SampleRate_e)spec->freq;
	attr.eSoundmode = (MI_AUDIO_SoundMode_e)(spec->channels - 1);
	attr.u32ChnCnt = spec->channels;
	attr.u32PtNumPerFrm = spec->samples;

	if (MI_AO_SetPubAttr(0,&attr)) return -1;
	if (MI_AO_Enable(0)) return -1;
	if (MI_AO_EnableChn(0,0)) return -1;
	if (MI_AO_SetMute(0,FALSE)) return -1;
	//if (MI_AO_SetVolume(0,0)) return -1;

	InitSettings(); // restores volume

	mixlen = spec->size;
	mixbuf = (Uint8 *)SDL_AllocAudioMem(mixlen);
	if ( mixbuf == NULL ) {
		MIAO_CloseAudio(this);
		return(-1);
	}
	memset(mixbuf, 0, mixlen);
	
#ifdef	YIELD_WAIT
	policy = sched_getscheduler(0);
	scprm.sched_priority = 0;
#endif
	
	frame = (MI_AUDIO_Frame_t*)SDL_AllocAudioMem(sizeof(MI_AUDIO_Frame_t));
	if ( frame == NULL ) {
		MIAO_CloseAudio(this);
		return(-1);
	}
	memset(frame, 0, sizeof(MI_AUDIO_Frame_t));
	frame->apVirAddr[0] = mixbuf;
	frame->u32Len = mixlen;
	
	fprintf(stdout, "buffer size: %d\n", mixlen);
	
	// Buffer initial frames (calculate at least 20ms)
	num_frames = (uint32_t)((spec->freq-1) / (50*spec->samples)) +1;
	if (num_frames < 2) num_frames = 2;
	MI_AO_ClearChnBuf(0,0);
	for (i=num_frames; i>0; i--) MI_AO_SendFrame(0, 0, frame, 0);
	
	clock_freqframes = spec->samples * 1000000;
	framecounter = 0;
	gettimeofday(&tod, NULL);
	startclock = tod.tv_usec + tod.tv_sec * 1000000;
	
	/* Get the parent process id (we're the parent of the audio thread) */
	parent = getpid();
	
	return(0);
}

/* ------------------------------------------------------------------------
 * "asrv": audio through Koriki's audioserver (audiofix ON).
 *
 * While audioserver owns the MI_AO device the miao driver is not available.
 * Programs started with LD_PRELOAD=libpadsp.so still reach audioserver through
 * libpadsp's OSS emulation (dsp driver), but a process that switches audiofix
 * ON while running (SimpleMenu) has no libpadsp loaded and stayed silent.
 * This driver talks to audioserver with the same protocol libpadsp uses:
 *   - PCM is written to /tmp/audio_fifo_server
 *   - an OSS ioctl is sent as {cmd, arg[16], size} (24 bytes) to
 *     /tmp/audio_fifo_ioctl_req and answered with {ret, errno, arg[16], pad}
 *     (28 bytes) on /tmp/audio_fifo_ioctl_res
 * It is only offered when libpadsp is NOT loaded, so programs that work
 * through libpadsp today keep exactly the same path.
 * ------------------------------------------------------------------------ */
#include <dlfcn.h>
#include <poll.h>
#include <sys/soundcard.h>

#define ASRV_DRIVER_NAME "asrv"
#define ASRV_FIFO        "/tmp/audio_fifo_server"
#define ASRV_REQ         "/tmp/audio_fifo_ioctl_req"
#define ASRV_RES         "/tmp/audio_fifo_ioctl_res"

static int asrv_fd = -1, asrv_req = -1, asrv_res = -1;

static void ASRV_CloseFds(void)
{
	if (asrv_fd >= 0)  close(asrv_fd);
	if (asrv_req >= 0) close(asrv_req);
	if (asrv_res >= 0) close(asrv_res);
	asrv_fd = asrv_req = asrv_res = -1;
}

/* one OSS ioctl through audioserver; returns its result, *value updated */
static int ASRV_Ioctl(unsigned int cmd, int *value)
{
	struct { Uint32 cmd; Uint8 arg[16]; Uint32 size; } req;
	struct { Sint32 ret; Sint32 err; Uint8 arg[16]; Uint32 pad; } res;
	struct pollfd pfd;

	SDL_memset(&req, 0, sizeof(req));
	req.cmd = cmd;
	SDL_memcpy(req.arg, value, sizeof(*value));
	req.size = sizeof(*value);
	if (write(asrv_req, &req, sizeof(req)) != (ssize_t)sizeof(req))
		return -1;

	pfd.fd = asrv_res;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, 500) <= 0)	/* audioserver not answering */
		return -1;
	if (read(asrv_res, &res, sizeof(res)) != (ssize_t)sizeof(res))
		return -1;
	if (res.ret >= 0)
		SDL_memcpy(value, res.arg, sizeof(*value));
	return res.ret;
}

static int ASRV_Available(void)
{
	/* libpadsp loaded in this process: the dsp driver already reaches audioserver */
	if (dlsym(RTLD_DEFAULT, "open_oss") != NULL)
		return 0;
	if (access(ASRV_FIFO, W_OK) || access(ASRV_REQ, W_OK) || access(ASRV_RES, R_OK))
		return 0;
	return 1;
}

static void ASRV_WaitAudio(_THIS)
{
	/* nothing: write() on the FIFO blocks while audioserver's buffer is full */
}

static void ASRV_PlayAudio(_THIS)
{
	Uint8 *p = mixbuf;
	int left = mixlen;

	while (left > 0) {
		ssize_t w = write(asrv_fd, p, left);
		if (w > 0) {
			p += w;
			left -= w;
		} else if (w < 0 && errno == EINTR) {
			continue;
		} else {
			perror("Audio write (audioserver)");
			this->enabled = 0;
			return;
		}
	}
}

static void ASRV_CloseAudio(_THIS)
{
	ASRV_CloseFds();
	if (mixbuf != NULL) {
		SDL_FreeAudioMem(mixbuf);
		mixbuf = NULL;
	}
}

static int ASRV_OpenAudio(_THIS, SDL_AudioSpec *spec)
{
	int fmt = AFMT_S16_LE, channels = spec->channels, rate = spec->freq;

	fprintf(stdout, "open audioserver audio\n");

	/* O_RDWR like libpadsp: opening a FIFO this way never blocks */
	asrv_fd  = open(ASRV_FIFO, O_RDWR);
	asrv_req = open(ASRV_REQ, O_RDWR);
	asrv_res = open(ASRV_RES, O_RDWR);
	if (asrv_fd < 0 || asrv_req < 0 || asrv_res < 0) {
		ASRV_CloseFds();
		SDL_SetError("Couldn't open audioserver FIFOs");
		return -1;
	}

	/* audioserver may have just been started (audiofix switched ON):
	   retry the negotiation for up to ~2 s */
	{
		int tries, ok = 0;
		for (tries = 0; tries < 4 && !ok; tries++) {
			fmt = AFMT_S16_LE; channels = spec->channels; rate = spec->freq;
			ok = ASRV_Ioctl(SNDCTL_DSP_SETFMT, &fmt) >= 0 && fmt == AFMT_S16_LE &&
			     ASRV_Ioctl(SNDCTL_DSP_CHANNELS, &channels) >= 0 &&
			     ASRV_Ioctl(SNDCTL_DSP_SPEED, &rate) >= 0;
		}
		if (!ok) {
			ASRV_CloseFds();
			SDL_SetError("audioserver rejected the audio format");
			return -1;
		}
	}

	spec->format = AUDIO_S16SYS;
	spec->channels = channels;
	spec->freq = rate;
	SDL_CalculateAudioSpec(spec);

	mixlen = spec->size;
	mixbuf = (Uint8 *)SDL_AllocAudioMem(mixlen);
	if (mixbuf == NULL) {
		ASRV_CloseFds();
		SDL_OutOfMemory();
		return -1;
	}
	SDL_memset(mixbuf, spec->silence, mixlen);
	parent = getpid();
	fprintf(stdout, "audioserver audio: %d Hz, %d ch, buffer %d\n", rate, channels, mixlen);
	return 0;
}

static SDL_AudioDevice *ASRV_CreateDevice(int devindex)
{
	SDL_AudioDevice *this = Audio_CreateDevice(devindex);

	if (this) {
		this->OpenAudio = ASRV_OpenAudio;
		this->WaitAudio = ASRV_WaitAudio;
		this->PlayAudio = ASRV_PlayAudio;
		this->GetAudioBuf = MIAO_GetAudioBuf;
		this->CloseAudio = ASRV_CloseAudio;
	}
	return this;
}

AudioBootStrap ASRV_bootstrap = {
	ASRV_DRIVER_NAME, "Koriki audioserver (audiofix) without libpadsp",
	ASRV_Available, ASRV_CreateDevice
};
