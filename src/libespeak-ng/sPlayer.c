#include <espeak-ng/espeak_ng.h>
#include <espeak-ng/speak_lib.h>
#include "sPlayer.h"

extern unsigned char *out_ptr;
extern unsigned char *out_end;

static speechPlayer_handle_t speechPlayerHandle=NULL;
static const unsigned int minFadeLength=110;

static int MIN(int a, int b) { return((a) < (b) ? a : b); }

static bool needsMixWaveFile(WGEN_DATA *wdata) {
	return (bool)wdata->n_mix_wavefile;
}

// mixes the currently queued espeak consonant wave file into the existing content in the given sample buffer.
// This would be used for voiced consonants where the voiced part is generated by speechPlayer, but the consonant comes from a wave file in eSpeak.
// e.g. z, v. 
// @param maxNumSamples the maximum number of samples that can be mixed into the sample buffer.
// @param sampleBuf the buffer of existing samples.
static void mixWaveFile(WGEN_DATA *wdata, unsigned int maxNumSamples, sample* sampleBuf) {
	unsigned int i=0;
	for(;wdata->mix_wavefile_ix<wdata->n_mix_wavefile;++wdata->mix_wavefile_ix) {
		if(i>=maxNumSamples) break;
		int val;
		if(wdata->mix_wave_scale==0) {
			val=wdata->mix_wavefile[wdata->mix_wavefile_ix+wdata->mix_wavefile_offset];
			++(wdata->mix_wavefile_ix);
			signed char c=wdata->mix_wavefile[wdata->mix_wavefile_ix+wdata->mix_wavefile_offset];
			val+=(c*256);
		} else {
			val=(signed char)wdata->mix_wavefile[wdata->mix_wavefile_ix+wdata->mix_wavefile_offset]*wdata->mix_wave_scale;
		}
		val*=(wdata->amplitude_v/1024.0);
		val=(val*wdata->mix_wave_amp)/40;
		sampleBuf[i].value+=val;
		if((wdata->mix_wavefile_ix+wdata->mix_wavefile_offset)>=wdata->mix_wavefile_max) {
			wdata->mix_wavefile_offset-=(wdata->mix_wavefile_max*3)/4;
		}
		++i;
	}
}

static bool isKlattFrameFollowing() {
	// eSpeak implements its command queue with a circular buffer.
	// Thus to walk it, we start from the head, walking to the tail, which may wrap around to the beginning of the buffer as it is circular.
	for(int i=(wcmdq_head+1)%N_WCMDQ;i!=wcmdq_tail;i=(i+1)%N_WCMDQ) {
		int cmd=wcmdq[i][0];
		if(cmd==WCMD_PAUSE||cmd==WCMD_WAVE) {
			break;
		}
		if(cmd==WCMD_KLATT) {
			return true;
		}
	}
	return false;
}

static void fillSpeechPlayerFrame(WGEN_DATA *wdata, voice_t *wvoice, frame_t * eFrame, speechPlayer_frame_t* spFrame) {
	// eSpeak stores pitch in 4096ths of a hz. Specifically comments in voice.h  mentions pitch<<12.
	// SpeechPlayer deals with floating point values  of hz.
	spFrame->voicePitch=(wdata->pitch)/4096.0;
	// eSpeak stores voicing amplitude with 64 representing 100% according to comments in voice.h.
	// speechPlayer uses floating point value of 1 as 100%.
	spFrame->voiceAmplitude=(wvoice->voicing)/64.0;
	spFrame->aspirationAmplitude=(wvoice->breath[1])/64.0;
	// All of eSpeak's relative formant frequency ratio values are stored with 256 representing 100% according to comments in voice.h. 
	spFrame->cf1=(eFrame->ffreq[1]*wvoice->freq[1]/256.0)+wvoice->freqadd[1];
	spFrame->cf2=(eFrame->ffreq[2]*wvoice->freq[2]/256.0)+wvoice->freqadd[2];
	spFrame->cf3=(eFrame->ffreq[3]*wvoice->freq[3]/256.0)+wvoice->freqadd[3];
	spFrame->cf4=(eFrame->ffreq[4]*wvoice->freq[4]/256.0)+wvoice->freqadd[4];
	spFrame->cf5=(eFrame->ffreq[5]*wvoice->freq[5]/256.0)+wvoice->freqadd[5];
	spFrame->cf6=(eFrame->ffreq[6]*wvoice->freq[6]/256.0)+wvoice->freqadd[6];
	spFrame->cfNP=200;
	spFrame->cfN0=250;
	if(eFrame->klattp[KLATT_FNZ]>0) {
		spFrame->caNP=1;
		spFrame->cfN0=eFrame->klattp[KLATT_FNZ]*2;
	} else {
		spFrame->caNP=0;
	}
	spFrame->cb1=eFrame->bw[0]*2*(wvoice->width[1]/256.0);
	spFrame->cb2=eFrame->bw[1]*2*(wvoice->width[2]/256.0);
	spFrame->cb3=eFrame->bw[2]*2*(wvoice->width[3]/256.0);
	spFrame->cb4=eFrame->bw[3]*2*(wvoice->width[4]/256.0);
	spFrame->cb5=1000;
	spFrame->cb6=1000;
	spFrame->cbNP=100;
	spFrame->cbN0=100;
	spFrame->preFormantGain=1;
	spFrame->outputGain=3*(wdata->amplitude/100.0);
	spFrame->endVoicePitch=spFrame->voicePitch;
}

void KlattInitSP() {
	speechPlayerHandle=speechPlayer_initialize(22050);
}

void KlattFiniSP() {
	if (speechPlayerHandle)
		speechPlayer_terminate(speechPlayerHandle);
	speechPlayerHandle = NULL;
}

void KlattResetSP() {
	KlattFiniSP();
	KlattInitSP();
}

int Wavegen_KlattSP(WGEN_DATA *wdata, voice_t *wvoice, int length, int resume, frame_t *fr1, frame_t *fr2){
	if(!resume) {
		speechPlayer_frame_t spFrame1={0};
		fillSpeechPlayerFrame(wdata, wvoice, fr1,&spFrame1);
		speechPlayer_frame_t spFrame2={0};
		fillSpeechPlayerFrame(wdata, wvoice, fr2,&spFrame2);
		wdata->pitch_ix+=(wdata->pitch_inc*(length/STEPSIZE));
		wdata->pitch=((wdata->pitch_env[MIN(wdata->pitch_ix>>8,127)]*wdata->pitch_range)>>8)+wdata->pitch_base;
		spFrame2.endVoicePitch=wdata->pitch/4096;
		bool willMixWaveFile=needsMixWaveFile(wdata);
		if(willMixWaveFile) {
			spFrame1.outputGain/=5;
			spFrame2.outputGain/=5;
		}
		int mainLength=length;
		speechPlayer_queueFrame(speechPlayerHandle,&spFrame1,minFadeLength,minFadeLength,-1,false);
		mainLength-=minFadeLength;
		bool fadeOut=!isKlattFrameFollowing();
		if(fadeOut) {
			mainLength-=minFadeLength;
		}
		if(mainLength>=1) {
			speechPlayer_queueFrame(speechPlayerHandle,&spFrame2,mainLength,mainLength,-1,false);
		}
		if(fadeOut) {
			spFrame2.voicePitch=spFrame2.endVoicePitch;
			spFrame2.preFormantGain=0;
			speechPlayer_queueFrame(speechPlayerHandle,&spFrame2,minFadeLength/2,minFadeLength/2,-1,false);
			spFrame2.outputGain=0;
			speechPlayer_queueFrame(speechPlayerHandle,&spFrame2,minFadeLength/2,minFadeLength/2,-1,false);
		}
	}
	unsigned int maxLength=(out_end-out_ptr)/sizeof(sample);
	unsigned int outLength=speechPlayer_synthesize(speechPlayerHandle,maxLength,(sample*)out_ptr);
	mixWaveFile(wdata, outLength,(sample*)out_ptr);
	out_ptr=out_ptr+(sizeof(sample)*outLength);
	if(out_ptr>=out_end) return 1;
	return 0;
}
