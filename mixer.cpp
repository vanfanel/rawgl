
/*
 * Another World engine rewrite
 * Copyright (C) 2004-2005 Gregory Montoir (cyx@users.sourceforge.net)
 */

#include <SDL2/SDL.h>
#include "file.h"
#include "mixer.h"
#include "sfxplayer.h"
#include "systemstub.h"
#include "util.h"

// This define is needed to have the actual functions implementations.
#define STS_MIXER_IMPLEMENTATION
#include "sts_mixer.h"

SDL_AudioDeviceID   audio_device = 0;
sts_mixer_t mixer;

sts_mixer_stream_t  stream;             // for the music
int8_t music_buffer[256*2];

static const float kGain = 1.f;
static const float kPitch = 1.f;
static const float kPan = 0.f;

// STS : SDL2 audio callback
static void audio_callback(void *userdata, Uint8* stream, int len) {
	sts_mixer_mix_audio(&mixer, stream, len / (sizeof(int16_t) * 2));
}

static void music_callback(sts_mixer_sample_t *sample, void *userdata) {
	// Here we fill-in the sample data. When it's depleted by sts_mixer, this fn is called again to refill it.
	((SfxPlayer *)userdata)->readSamples((int8_t *)sample->data, sample->length / 2);
}

static uint8_t *convertMono8ToWav(const uint8_t *data, int freq, int size, const uint8_t mask = 0) {
	static const uint8_t kHeaderData[] = {
		0x52, 0x49, 0x46, 0x46, 0x24, 0x00, 0x00, 0x00, 0x57, 0x41, 0x56, 0x45, 0x66, 0x6d, 0x74, 0x20, // RIFF$...WAVEfmt
		0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x7d, 0x00, 0x00, 0x00, 0x7d, 0x00, 0x00, // .........}...}..
		0x01, 0x00, 0x08, 0x00, 0x64, 0x61, 0x74, 0x61, 0x00, 0x00, 0x00, 0x00                          // ....data....
	};
	static const int kHz = 11025;
	uint8_t *out = (uint8_t *)malloc(sizeof(kHeaderData) + size * 4);
	if (out) {
		memcpy(out, kHeaderData, sizeof(kHeaderData));
		// point resampling
		Frac pos;
		pos.offset = 0;
		pos.inc = (freq << Frac::BITS) / kHz;
		int rsmp = 0;
		for (; int(pos.getInt()) < size; pos.offset += pos.inc) {
			out[sizeof(kHeaderData) + rsmp] = data[pos.getInt()] ^ mask; // S8 to U8
			++rsmp;
		}
		// fixup .wav header
		WRITE_LE_UINT32(out +  4, 36 + rsmp); // 'RIFF' size
		WRITE_LE_UINT32(out + 24, kHz); // 'fmt ' sample rate
		WRITE_LE_UINT32(out + 28, kHz); // 'fmt ' bytes per second
		WRITE_LE_UINT32(out + 40, rsmp); // 'data' size
	}
	return out;
}

struct Mixer_impl {

	static const int kMixFreq = 44100;

        // This is the number of chanels for the sts mixer.
        // We create this number of _channels structs.
        static const int kPcmChannels = 4;

	struct {
		int voice;
		uint8_t channel;
		uint8_t *sample_buffer;
		sts_mixer_sample_t sample;
	} _channels[kPcmChannels];

	void init() {

                SDL_AudioSpec       want, have;

		/* init SDL2 + audio */
		want.format = AUDIO_S16SYS;
		want.freq = kMixFreq;
		want.channels = 2;
		want.userdata = NULL;
		want.samples = 512;
		want.callback = audio_callback;

		audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);

		sts_mixer_init(&mixer, have.freq, STS_MIXER_SAMPLE_FORMAT_16);

                /* Silence all channels, and assign a channel number (0 to 3) to each _channel.
		   This channel number won't change, and when the game wants to do something with
		   channel n, we will refer to the channel in _channels[] by it's index. */
		memset(_channels, 0, sizeof(_channels));
		for (int i = 0; i < kPcmChannels; ++i) {
			_channels[i].voice   = -1;
			_channels[i].channel =  i;
		}

                /* Start consuming samples, and calling the SDL2 callback to refill the buffer when
                   the buffer is consumed. */ 			      
                SDL_PauseAudioDevice(audio_device, 0);
	}
	void quit() {
		stopAll();

		SDL_PauseAudioDevice(audio_device, 1);
		SDL_CloseAudioDevice(audio_device);
	}

	void update() {
	}

	void playSoundRaw(uint8_t channel, const uint8_t *data, int freq, uint8_t volume) {

		/* This function just builds a WAV file out of the raw sample, and then sends it to be
		   played by playSoundWav(). */
		int len = READ_BE_UINT16(data) * 2;
		const int loopLen = READ_BE_UINT16(data + 2) * 2;
		if (loopLen != 0) {
			len = loopLen;
		}
		
		uint8_t *sample = convertMono8ToWav(data + 8, freq, len);

		if (sample) {
			playSoundWav(channel, sample, 0, volume, (loopLen != 0) ? -1 : 0);
		}

		/* DONT FREE THE SAMPLE BUFFER HERE, because sts_play_sample() returns immediately but
		   we dont know for how long will sts_mix_audio be accessing the buffer to mix the
		   sample into the mixing stream. */
	}
	void playSoundWav(uint8_t channel, const uint8_t *data, int freq, uint8_t volume, int loops = 0) {

		const uint8_t *pcm = data + 44;
		const uint8_t *fmt = data + 20;
		const int frequency = READ_LE_UINT32(fmt + 4);
		const uint32_t size = READ_LE_UINT32(data + 4) + 8;

		/* We must lock access to every variable that is accessed from the sts_mixer side. */	
		SDL_LockAudioDevice(audio_device);

		/* We keep this pointer in each channel structure because the channel.sample.data pointer
		   will be moved by sts_mixer as sound is played. We need the starting address so we can free
		   the sample memory in freeSound() */
		_channels[channel].sample_buffer = (uint8_t *) data;

		_channels[channel].sample.length = size / sizeof(int8_t);
		_channels[channel].sample.frequency = frequency;
		/* sts_mixer supports SIGNED 8bit samples ONLY */
		_channels[channel].sample.audio_format = STS_MIXER_SAMPLE_FORMAT_8; 
		_channels[channel].sample.data = (void *) pcm;
		_channels[channel].sample.loops = loops;
		_channels[channel].sample.loops_done = 0;

		//sts_mixer_stop_voice(&mixer, _channels[free_channel].voice);
		_channels[channel].voice = sts_mixer_play_sample(&mixer, &(_channels[channel].sample), kGain, kPitch, kPan);
		SDL_UnlockAudioDevice(audio_device);

	}
	void stopSound(uint8_t channel) {
    		SDL_LockAudioDevice(audio_device);
		sts_mixer_stop_voice(&mixer, _channels[channel].voice);
		freeSound(channel);
    		SDL_UnlockAudioDevice(audio_device);
	}
	void freeSound(int channel) {
		/* We check the channel to see if its voice is STS_MIXER_VOICE_STOPPED and 
		   is NOT set to -1. Both conditions would mean the voice is stopped from the
		   sts_mixer side of things, but not from this side: we have to free the sample
		   buffer and set the voice to -1 in that case, so we KNOW the channel is stopped
		   from the rawgl side, too! */
		int voice = _channels[channel].voice;
		if (!(voice < 0) && mixer.voices[voice].state == STS_MIXER_VOICE_STOPPED) {
			if (_channels[channel].sample_buffer) {
				free (_channels[channel].sample_buffer);
				memset(&_channels[channel], 0, sizeof(sts_mixer_sample_t));
				_channels[channel].voice = -1;
			}			
		}
	}
	void setChannelVolume(uint8_t channel, uint8_t volume) {
	}

	static void mixSfxPlayer(void *data, uint8_t *s16buf, int len) {
		len /= 2;
		int8_t *s8buf = (int8_t *)alloca(len);
		memset(s8buf, 0, len);

		((SfxPlayer *)data)->readSamples(s8buf, len / 2);

		for (int i = 0; i < len; ++i) {
			*(int16_t *)&s16buf[i * 2] = 256 * (int16_t)s8buf[i];
		}
	}

	void playSfxMusic(SfxPlayer *sfx) {

                stream.sample.frequency =    sfx->_rate;
		stream.sample.audio_format = STS_MIXER_SAMPLE_FORMAT_8;
		stream.sample.length =       256*2;
		stream.sample.data =         music_buffer;
		stream.callback =            music_callback;
                stream.userdata =            sfx;

		/* Start consuming music samples and calling the music_callback function when needed to refill */
		sts_mixer_play_stream(&mixer, &stream, 1.0f);
	}
	void stopSfxMusic() {
		sts_mixer_stop_stream(&mixer, &stream);
	}

	void stopAll() {
		for (int i = 0; i < 4; ++i) {
			stopSound(i);
		}
		stopSfxMusic();
	}
};

Mixer::Mixer(SfxPlayer *sfx)
	: _sfx(sfx) {
}

void Mixer::init() {
	_impl = new Mixer_impl();
	_impl->init();
}

void Mixer::quit() {
	stopAll();
	if (_impl) {
		_impl->quit();
		delete _impl;
	}
}

void Mixer::update() {
	if (_impl) {
		_impl->update();
	}
}

void Mixer::playSoundRaw(uint8_t channel, const uint8_t *data, uint16_t freq, uint8_t volume) {
	debug(DBG_SND, "Mixer::playChannel(%d, %d, %d)", channel, freq, volume);
	if (_impl) {
		return _impl->playSoundRaw(channel, data, freq, volume);
	}
}

void Mixer::playSoundWav(uint8_t channel, const uint8_t *data, uint16_t freq, uint8_t volume) {
	debug(DBG_SND, "Mixer::playSoundWav(%d, %d)", channel, volume);
	if (_impl) {
		return _impl->playSoundWav(channel, data, freq, volume);
	}
}

void Mixer::playSoundAiff(uint8_t channel, const uint8_t *data, uint8_t volume) {
}

void Mixer::stopSound(uint8_t channel) {
	debug(DBG_SND, "Mixer::stopChannel(%d)", channel);
	if (_impl) {
		return _impl->stopSound(channel);
	}
}

void Mixer::setChannelVolume(uint8_t channel, uint8_t volume) {
	debug(DBG_SND, "Mixer::setChannelVolume(%d, %d)", channel, volume);
	if (_impl) {
		return _impl->setChannelVolume(channel, volume);
	}
}

void Mixer::playMusic(const char *path) {
	if (_impl) {
		return;
	}
}

void Mixer::stopMusic() {
	if (_impl) {
		return;
	}
}

void Mixer::playAifcMusic(const char *path, uint32_t offset) {
}

void Mixer::stopAifcMusic() {
}

void Mixer::playSfxMusic(int num) {
	debug(DBG_SND, "Mixer::playSfxMusic(%d)", num);
	if (_impl && _sfx) {
		_sfx->play(Mixer_impl::kMixFreq);
		return _impl->playSfxMusic(_sfx);
	}
}

void Mixer::stopSfxMusic() {
	debug(DBG_SND, "Mixer::stopSfxMusic()");
	if (_impl && _sfx) {
		_sfx->stop();
		return _impl->stopSfxMusic();
	}
}

void Mixer::stopAll() {
	debug(DBG_SND, "Mixer::stopAll()");
	if (_impl) {
		return _impl->stopAll();
	}
}
