/*
** Copyright 2008, The Android Open-Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#include <math.h>

//#define LOG_NDEBUG 0
#define LOG_TAG "AudioHardware"

#include <utils/Log.h>
#include <utils/String8.h>

#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <fcntl.h>

#include "AudioHardware.h"
#include <media/AudioRecord.h>

extern "C" {
#include "msm_audio.h"
}

#define __force
#define __bitwise
#define __user
#include "asound.h"

static inline int alsa_param_is_mask(int p)
{
    return (p >= SNDRV_PCM_HW_PARAM_FIRST_MASK) &&
           (p <= SNDRV_PCM_HW_PARAM_LAST_MASK);
}

static inline int alsa_param_is_interval(int p)
{
    return (p >= SNDRV_PCM_HW_PARAM_FIRST_INTERVAL) &&
           (p <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL);
}

static inline struct snd_interval *alsa_param_to_interval(struct snd_pcm_hw_params *p, int n)
{
    return &(p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL]);
}

static inline struct snd_mask *alsa_param_to_mask(struct snd_pcm_hw_params *p, int n)
{
    return &(p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK]);
}

static void alsa_param_set_mask(struct snd_pcm_hw_params *p, int n, unsigned bit)
{
    if (bit >= SNDRV_MASK_MAX) return;
    if (alsa_param_is_mask(n)) {
        struct snd_mask *m = alsa_param_to_mask(p, n);
        m->bits[0] = 0;
        m->bits[1] = 0;
        m->bits[bit >> 5] |= (1 << (bit & 31));
    }
}

static void alsa_param_set_min(struct snd_pcm_hw_params *p, int n, unsigned val)
{
    if (alsa_param_is_interval(n)) {
        struct snd_interval *i = alsa_param_to_interval(p, n);
        i->min = val;
    }
}

static void alsa_param_set_int(struct snd_pcm_hw_params *p, int n, unsigned val)
{
    if (alsa_param_is_interval(n)) {
        struct snd_interval *i = alsa_param_to_interval(p, n);
        i->min = val;
        i->max = val;
        i->integer = 1;
    }
}

static void alsa_param_init(struct snd_pcm_hw_params *p)
{
    int n;
    memset(p, 0, sizeof(*p));
    for (n = SNDRV_PCM_HW_PARAM_FIRST_MASK; n <= SNDRV_PCM_HW_PARAM_LAST_MASK; n++) {
        struct snd_mask *m = alsa_param_to_mask(p, n);
        m->bits[0] = ~0;
        m->bits[1] = ~0;
    }
    for (n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; n++) {
        struct snd_interval *i = alsa_param_to_interval(p, n);
        i->min = 0;
        i->max = ~0;
    }
}


#define SND_IOCTL_MAGIC 's'
#define SND_SET_DEVICE _IOW(SND_IOCTL_MAGIC, 2, struct msm_snd_device_config *)
#define SND_SET_VOLUME _IOW(SND_IOCTL_MAGIC, 3, struct msm_snd_volume_config *)

#define SND_DEVICE_CURRENT             -1
#define SND_DEVICE_HANDSET             0
#define SND_DEVICE_SPEAKER             6
#define SND_DEVICE_HEADSET             3
#define SND_DEVICE_BT                  12
#define SND_DEVICE_CARKIT              4
#define SND_DEVICE_TTY_FULL            8
#define SND_DEVICE_TTY_VCO             9
#define SND_DEVICE_TTY_HCO             10
#define SND_DEVICE_NO_MIC_HEADSET      3
#define SND_DEVICE_FM_HEADSET          26
#define SND_DEVICE_HEADSET_AND_SPEAKER 31
#define SND_DEVICE_FM_SPEAKER          27

#define SND_MUTE_UNMUTED 0
#define SND_MUTE_MUTED   1

namespace android {

static uint32_t get_snd_device(uint32_t devices)
{
    if ((devices & AudioSystem::DEVICE_OUT_SPEAKER) &&
        (devices & (AudioSystem::DEVICE_OUT_WIRED_HEADSET | AudioSystem::DEVICE_OUT_WIRED_HEADPHONE))) {
        return SND_DEVICE_HEADSET_AND_SPEAKER;
    } else if (devices & (AudioSystem::DEVICE_OUT_WIRED_HEADSET | AudioSystem::DEVICE_OUT_WIRED_HEADPHONE)) {
        return SND_DEVICE_HEADSET;
    } else if (devices & AudioSystem::DEVICE_OUT_SPEAKER) {
        return SND_DEVICE_SPEAKER;
    } else if (devices & AudioSystem::DEVICE_OUT_EARPIECE) {
        return SND_DEVICE_HANDSET;
    } else if (devices & AudioSystem::DEVICE_OUT_BLUETOOTH_SCO) {
        return SND_DEVICE_BT;
    }
    return SND_DEVICE_SPEAKER;
}


static uint32_t get_in_snd_device(uint32_t devices)
{
    if (devices & AudioSystem::DEVICE_IN_WIRED_HEADSET) {
        return SND_DEVICE_HEADSET;
    } else if (devices & AudioSystem::DEVICE_IN_BLUETOOTH_SCO_HEADSET) {
        return SND_DEVICE_BT;
    } else if (devices & AudioSystem::DEVICE_IN_BACK_MIC) {
        return SND_DEVICE_SPEAKER;
    }
    return SND_DEVICE_HANDSET;
}

static bool is_valid_in_sample_rate(uint32_t rate)
{
    switch (rate) {
    case 8000:
    case 11025:
    case 12000:
    case 16000:
    case 22050:
    case 24000:
    case 32000:
    case 44100:
    case 48000:
        return true;
    default:
        return false;
    }
}

static int do_route_audio_rpc(uint32_t device, int ear_mute, int mic_mute)
{
    int fd = open("/dev/msm_snd", O_RDWR);
    if (fd < 0) {
        LOGE("Cannot open /dev/msm_snd errno: %d", errno);
        return -1;
    }

    struct msm_snd_device_config args;
    args.device = device;
    args.ear_mute = ear_mute ? SND_MUTE_MUTED : SND_MUTE_UNMUTED;
    args.mic_mute = mic_mute ? SND_MUTE_MUTED : SND_MUTE_UNMUTED;

    LOGI("do_route_audio_rpc: routing to device %u (ear_mute=%d, mic_mute=%d)",
         device, ear_mute, mic_mute);

    if (ioctl(fd, SND_SET_DEVICE, &args) < 0) {
        LOGE("ioctl SND_SET_DEVICE failed, errno: %d", errno);
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

// ----------------------------------------------------------------------------

AudioHardware::AudioHardware() :
    mInit(false), mMicMute(false), mOutput(0), mInput(0), mCachedInFd(-1), mCachedInRate(0), mCachedInCh(0)
{
    mInit = true;
}

AudioHardware::~AudioHardware()
{
    closeOutputStream((AudioStreamOut*)mOutput);
    closeInputStream((AudioStreamIn*)mInput);
    if (mCachedInFd >= 0) { ::close(mCachedInFd); mCachedInFd = -1; }
    mInit = false;
}

status_t AudioHardware::initCheck()
{
    return mInit ? NO_ERROR : NO_INIT;
}

AudioStreamOut* AudioHardware::openOutputStream(
        uint32_t devices, int *format, uint32_t *channels, uint32_t *sampleRate, status_t *status)
{
    { // scope for the lock
        Mutex::Autolock lock(mLock);

        // only one output stream allowed
        if (mOutput) {
            if (status) {
                *status = INVALID_OPERATION;
            }
            return 0;
        }

        AudioStreamOutQ5V2* out = new AudioStreamOutQ5V2();

        status_t rc = out->set(this, devices, format, channels, sampleRate);
        if (rc) {
            *status = rc;
        }
        if (rc == NO_ERROR) {
            mOutput = out;
        } else {
            delete out;
        }
    }
    return mOutput;
}

void AudioHardware::closeOutputStream(AudioStreamOut* out) {
    Mutex::Autolock lock(mLock);
    if (mOutput == 0 || mOutput != out) {
        LOGW("Attempt to close invalid output stream");
    }
    else {
        delete mOutput;
        mOutput = 0;
    }
}

AudioStreamIn* AudioHardware::openInputStream(
        uint32_t devices, int *format, uint32_t *channels, uint32_t *sampleRate, status_t *status,
        AudioSystem::audio_in_acoustics acoustic_flags)
{
    Mutex::Autolock lock(mLock);
    if (!AudioSystem::isInputDevice((AudioSystem::audio_devices)devices)) {
        if (status) *status = BAD_VALUE;
        return 0;
    }
    if (mInput) {
        if (status) *status = INVALID_OPERATION;
        return 0;
    }
    AudioStreamInQ5V2* in = new AudioStreamInQ5V2();
    status_t rc = in->set(this, devices, format, channels, sampleRate, acoustic_flags);
    if (status) *status = rc;
    if (rc == NO_ERROR) {
        mInput = in;
    } else {
        delete in;
        in = 0;
    }
    return mInput;
}

void AudioHardware::closeInputStream(AudioStreamIn* in) {
    Mutex::Autolock lock(mLock);
    if (mInput == 0 || mInput != in) {
        LOGW("Attempt to close invalid input stream");
    } else {
        delete mInput;
        mInput = 0;
    }
}

status_t AudioHardware::setMode(int mode)
{
    return NO_ERROR;
}

status_t AudioHardware::setMicMute(bool state)
{
    mMicMute = state;
    return NO_ERROR;
}

status_t AudioHardware::getMicMute(bool* state)
{
    *state = mMicMute;
    return NO_ERROR;
}

status_t AudioHardware::setParameters(const String8& keyValuePairs)
{
    return NO_ERROR;
}

String8 AudioHardware::getParameters(const String8& keys)
{
    AudioParameter request = AudioParameter(keys);
    AudioParameter reply = AudioParameter();

    LOGV("getParameters() %s", keys.string());

    return reply.toString();
}

size_t AudioHardware::getInputBufferSize(uint32_t sampleRate, int format, int channelCount)
{
    if (format != AUDIO_HW_IN_FORMAT) return 0;
    if (channelCount != 1 && channelCount != 2) return 0;
    if (!is_valid_in_sample_rate(sampleRate)) return 0;
    return AUDIO_HW_IN_BUFSZ;
}

status_t AudioHardware::setVoiceVolume(float v)
{
    return NO_ERROR;
}

status_t AudioHardware::setMasterVolume(float v)
{
    LOGI("Set master volume to %f.\n", v);
    return -1;
}

status_t AudioHardware::dump(int fd, const Vector<String16>& args)
{
    return NO_ERROR;
}

AudioHardware::AudioStreamOutQ5V2::AudioStreamOutQ5V2() :
    mHardware(0), mFd(-1), mStartCount(0), mRetryCount(0), mStandby(true),
    mDevices(0), mChannels(AUDIO_HW_OUT_CHANNELS), mSampleRate(AUDIO_HW_OUT_SAMPLERATE),
    mBufferSize(AUDIO_HW_OUT_BUFSZ)
{
}

status_t AudioHardware::AudioStreamOutQ5V2::set(
        AudioHardware* hw, uint32_t devices, int *pFormat, uint32_t *pChannels, uint32_t *pRate)
{
    int lFormat = pFormat ? *pFormat : 0;
    uint32_t lChannels = pChannels ? *pChannels : 0;
    uint32_t lRate = pRate ? *pRate : 0;

    mHardware = hw;
    mDevices = devices;

    // fix up defaults
    if (lFormat == 0) lFormat = format();
    if (lChannels == 0) lChannels = channels();
    if (lRate == 0) lRate = sampleRate();

    // check values
    if ((lFormat != format()) ||
        (lChannels != channels()) ||
        (lRate != sampleRate())) {
        if (pFormat) *pFormat = format();
        if (pChannels) *pChannels = channels();
        if (pRate) *pRate = sampleRate();
        return BAD_VALUE;
    }

    if (pFormat) *pFormat = lFormat;
    if (pChannels) *pChannels = lChannels;
    if (pRate) *pRate = lRate;

    mChannels = lChannels;
    mSampleRate = lRate;
    mBufferSize = 4096;

    return NO_ERROR;
}

AudioHardware::AudioStreamOutQ5V2::~AudioStreamOutQ5V2()
{
    standby();
}

ssize_t AudioHardware::AudioStreamOutQ5V2::write(const void* buffer, size_t bytes)
{
    status_t status = NO_INIT;
    size_t count = bytes;
    const uint8_t* p = static_cast<const uint8_t*>(buffer);

    if (mStandby) {
        LOGV("open pcm_out driver");
        status = ::open("/dev/msm_pcm_out", O_RDWR);
        if (status < 0) {
            LOGE("Cannot open /dev/msm_pcm_out errno: %d", errno);
            goto Error;
        }
        mFd = status;

        // Route audio device
        uint32_t snd_dev = get_snd_device(mDevices);
        do_route_audio_rpc(snd_dev, 0, 0);

        // configuration
        LOGV("get config");
        struct msm_audio_config config;
        status = ioctl(mFd, AUDIO_GET_CONFIG, &config);
        if (status < 0) {
            LOGE("Cannot read pcm_out config");
            goto Error;
        }

        LOGV("set pcm_out config");
        config.channel_count = AudioSystem::popCount(channels());
        config.sample_rate = mSampleRate;
        config.buffer_size = mBufferSize;
        config.buffer_count = AUDIO_HW_NUM_OUT_BUF;
        status = ioctl(mFd, AUDIO_SET_CONFIG, &config);
        if (status < 0) {
            LOGE("Cannot set config");
            goto Error;
        }

        // fill buffers before AUDIO_START
        mStartCount = AUDIO_HW_NUM_OUT_BUF;
        mStandby = false;
    }

    while (count) {
        ssize_t written = ::write(mFd, p, count);
        if (written >= 0) {
            count -= written;
            p += written;
        } else {
            if (errno != EAGAIN) return written;
            mRetryCount++;
            LOGW("EAGAIN - retry");
        }
    }

    // start audio after pre-filling buffers
    if (mStartCount) {
        if (--mStartCount == 0) {
            LOGI("AudioStreamOutQ5V2: calling AUDIO_START ioctl");
            if (ioctl(mFd, AUDIO_START, 0) < 0) {
                LOGE("AudioStreamOutQ5V2: AUDIO_START failed, errno: %d", errno);
            }
        }
    }

    return bytes;

Error:
    if (mFd >= 0) {
        ::close(mFd);
        mFd = -1;
    }
    // Simulate audio output timing in case of error
    usleep(bytes * 1000000 / frameSize() / sampleRate());

    return status;
}

status_t AudioHardware::AudioStreamOutQ5V2::standby()
{
    status_t status = NO_ERROR;
    if (!mStandby && mFd >= 0) {
        ::close(mFd);
        mFd = -1;
    }
    mStandby = true;
    LOGI("AudioHardware pcm playback is going to standby.");
    return status;
}

status_t AudioHardware::AudioStreamOutQ5V2::dump(int fd, const Vector<String16>& args)
{
    return NO_ERROR;
}

bool AudioHardware::AudioStreamOutQ5V2::checkStandby()
{
    return mStandby;
}

status_t AudioHardware::AudioStreamOutQ5V2::setParameters(const String8& keyValuePairs)
{
    AudioParameter param = AudioParameter(keyValuePairs);
    int value;
    if (param.getInt(String8(AudioParameter::keyRouting), value) == NO_ERROR) {
        mDevices = (uint32_t)value;
        if (!mStandby) {
            uint32_t snd_dev = get_snd_device(mDevices);
            do_route_audio_rpc(snd_dev, 0, 0);
        }
    }
    return NO_ERROR;
}

String8 AudioHardware::AudioStreamOutQ5V2::getParameters(const String8& keys)
{
    AudioParameter param = AudioParameter(keys);
    LOGV("AudioStreamOutQ5V2::getParameters() %s", param.toString().string());
    return param.toString();
}

status_t AudioHardware::AudioStreamOutQ5V2::getRenderPosition(uint32_t *dspFrames)
{
    return INVALID_OPERATION;
}


// ----------------------------------------------------------------------------

AudioHardware::AudioStreamInQ5V2::AudioStreamInQ5V2() :
    mHardware(0), mFd(-1), mStandby(true), mStarted(false), mRouted(false), mRetryCount(0),
    mDevices(0), mChannels(AUDIO_HW_IN_CHANNELS),
    mSampleRate(AUDIO_HW_IN_SAMPLERATE), mBufferSize(AUDIO_HW_IN_BUFSZ),
    mAcoustics((AudioSystem::audio_in_acoustics)0)
{
}

AudioHardware::AudioStreamInQ5V2::~AudioStreamInQ5V2()
{
    standby();
}

status_t AudioHardware::AudioStreamInQ5V2::set(
        AudioHardware* hw, uint32_t devices, int *pFormat, uint32_t *pChannels, uint32_t *pRate,
        AudioSystem::audio_in_acoustics acoustics)
{
    int lFormat = pFormat ? *pFormat : 0;
    uint32_t lChannels = pChannels ? *pChannels : 0;
    uint32_t lRate = pRate ? *pRate : 0;

    if (lFormat == 0) lFormat = AUDIO_HW_IN_FORMAT;
    if (lChannels == 0) lChannels = AUDIO_HW_IN_CHANNELS;
    if (lRate == 0) lRate = AUDIO_HW_IN_SAMPLERATE;

    if (lFormat != AUDIO_HW_IN_FORMAT) {
        if (pFormat) *pFormat = AUDIO_HW_IN_FORMAT;
        return BAD_VALUE;
    }
    if (lChannels != AudioSystem::CHANNEL_IN_MONO &&
        lChannels != AudioSystem::CHANNEL_IN_STEREO) {
        if (pChannels) *pChannels = AUDIO_HW_IN_CHANNELS;
        return BAD_VALUE;
    }
    uint32_t targetRate = (hw && hw->mCachedInFd >= 0) ? hw->mCachedInRate : 16000;
    if (lRate != targetRate) {
        if (pRate) *pRate = targetRate;
        return BAD_VALUE;
    }

    if (pFormat) *pFormat = lFormat;
    if (pChannels) *pChannels = lChannels;
    if (pRate) *pRate = lRate;

    mHardware = hw;
    mDevices = devices;
    mChannels = lChannels;
    mSampleRate = lRate;
    mAcoustics = acoustics;
    mBufferSize = AUDIO_HW_IN_BUFSZ;

    /* Pre-open & pre-route capture driver before AudioSource::read captures initial timestamp. */
    status_t rc = openDriver();
    if (rc != NO_ERROR) {
        return rc;
    }

    return NO_ERROR;
}

status_t AudioHardware::AudioStreamInQ5V2::openDriver()
{
    if (mFd >= 0) {
        return NO_ERROR;
    }
    if (mHardware && mHardware->mCachedInFd >= 0) {
        if (mHardware->mCachedInRate == mSampleRate && mHardware->mCachedInCh == mChannels) {
            mFd = mHardware->mCachedInFd;
            do_route_audio_rpc(get_in_snd_device(mDevices), 0, mHardware->mMicMute ? 1 : 0);
            mRouted = true;
            mStandby = false;
            mStarted = false;
            LOGI("AudioStreamInQ5V2: reusing cached /dev/snd/pcmC0D0c fd=%d rate=%u", mFd, mSampleRate);
            return NO_ERROR;
        }
        ::close(mHardware->mCachedInFd);
        mHardware->mCachedInFd = -1;
    }
    int fd = ::open("/dev/snd/pcmC0D0c", O_RDWR);
    if (fd < 0) {
        LOGE("Cannot open /dev/snd/pcmC0D0c errno: %d", errno);
        return -errno;
    }

    uint32_t ch = AudioSystem::popCount(mChannels);
    uint32_t frame_bytes = ch * 2;
    uint32_t period_frames = AUDIO_HW_IN_BUFSZ / frame_bytes;
    uint32_t periods = 4;

    struct snd_pcm_hw_params params;
    alsa_param_init(&params);
    alsa_param_set_mask(&params, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    alsa_param_set_mask(&params, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    alsa_param_set_mask(&params, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
    alsa_param_set_int(&params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16);
    alsa_param_set_int(&params, SNDRV_PCM_HW_PARAM_FRAME_BITS, ch * 16);
    alsa_param_set_int(&params, SNDRV_PCM_HW_PARAM_CHANNELS, ch);
    alsa_param_set_int(&params, SNDRV_PCM_HW_PARAM_RATE, mSampleRate);
    alsa_param_set_min(&params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, period_frames);
    alsa_param_set_int(&params, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, AUDIO_HW_IN_BUFSZ);
    alsa_param_set_int(&params, SNDRV_PCM_HW_PARAM_PERIODS, periods);

    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &params) < 0) {
        LOGE("pcmC0D0c SNDRV_PCM_IOCTL_HW_PARAMS failed (ch=%u rate=%u): errno=%d",
             ch, mSampleRate, errno);
        ::close(fd);
        return -errno;
    }

    struct snd_pcm_sw_params sparams;
    memset(&sparams, 0, sizeof(sparams));
    sparams.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    sparams.period_step = 1;
    sparams.avail_min = 1;
    sparams.start_threshold = periods * period_frames;
    sparams.stop_threshold = periods * period_frames;
    sparams.xfer_align = period_frames / 2;
    sparams.silence_size = 0;
    sparams.silence_threshold = 0;

    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sparams) < 0) {
        LOGE("pcmC0D0c SNDRV_PCM_IOCTL_SW_PARAMS failed: errno=%d", errno);
        ::close(fd);
        return -errno;
    }

    mBufferSize = AUDIO_HW_IN_BUFSZ;
    mFd = fd;
    if (mHardware) {
        mHardware->mCachedInFd = fd;
        mHardware->mCachedInRate = mSampleRate;
        mHardware->mCachedInCh = mChannels;
    }
    do_route_audio_rpc(get_in_snd_device(mDevices), 0, mHardware && mHardware->mMicMute ? 1 : 0);
    mRouted = true;
    mStandby = false;
    mStarted = false;
    LOGI("AudioStreamInQ5V2: opened /dev/snd/pcmC0D0c rate=%u ch=%u buf=%zu dev=0x%x",
         mSampleRate, ch, mBufferSize, mDevices);
    return NO_ERROR;
}

ssize_t AudioHardware::AudioStreamInQ5V2::read(void* buffer, ssize_t bytes)
{
    if (!mHardware || bytes <= 0) return -1;
    if (mStandby || mFd < 0) {
        status_t rc = openDriver();
        if (rc != NO_ERROR) {
            usleep(bytes * 1000000 / frameSize() / sampleRate());
            return rc;
        }
    }
    if (!mStarted) {
        uint32_t snd_dev = get_in_snd_device(mDevices);
        if (!mRouted) {
            do_route_audio_rpc(snd_dev, 0, mHardware->mMicMute ? 1 : 0);
            mRouted = true;
        }
        if (ioctl(mFd, SNDRV_PCM_IOCTL_PREPARE, 0) < 0) {
            LOGE("pcmC0D0c SNDRV_PCM_IOCTL_PREPARE failed errno=%d", errno);
            standby();
            return -1;
        }
        if (ioctl(mFd, SNDRV_PCM_IOCTL_START, 0) < 0) {
            LOGE("pcmC0D0c SNDRV_PCM_IOCTL_START failed errno=%d", errno);
            standby();
            return -1;
        }
        LOGI("AudioStreamInQ5V2: ALSA capture started (snd_dev=%u, rate=%u)", snd_dev, mSampleRate);
        mStarted = true;
    }

    uint32_t frame_bytes = frameSize();
    uint32_t frames = (uint32_t)bytes / frame_bytes;
    struct snd_xferi x;
    x.buf = buffer;
    x.frames = frames;
    x.result = 0;

    for (int retries = 0; retries < 3; retries++) {
        if (ioctl(mFd, SNDRV_PCM_IOCTL_READI_FRAMES, &x) == 0) {
            return (ssize_t)(frames * frame_bytes);
        }
        if (errno == EPIPE) {
            LOGW("pcmC0D0c overrun (EPIPE), re-preparing");
            ioctl(mFd, SNDRV_PCM_IOCTL_PREPARE, 0);
            ioctl(mFd, SNDRV_PCM_IOCTL_START, 0);
            continue;
        }
        LOGE("pcmC0D0c SNDRV_PCM_IOCTL_READI_FRAMES failed errno=%d", errno);
        return -errno;
    }
    return -1;
}

status_t AudioHardware::AudioStreamInQ5V2::standby()
{
    if (mFd >= 0) {
        /* Keep /dev/snd/pcmC0D0c open across recordings because kernel msm_pcm_capture_close()
         * leaves QDSP5 AUDRECTASK in a wedged state on reopen. */
        if (!mHardware || mHardware->mCachedInFd != mFd) {
            ::close(mFd);
        }
        mFd = -1;
    }
    mStarted = false;
    mRouted = false;
    mStandby = true;
    return NO_ERROR;
}

status_t AudioHardware::AudioStreamInQ5V2::dump(int fd, const Vector<String16>& args)
{
    return NO_ERROR;
}

status_t AudioHardware::AudioStreamInQ5V2::setParameters(const String8& keyValuePairs)
{
    AudioParameter param = AudioParameter(keyValuePairs);
    int value;
    if (param.getInt(String8(AudioParameter::keyRouting), value) == NO_ERROR) {
        if (value != 0) {
            mDevices = (uint32_t)value;
            if (!mStandby) {
                do_route_audio_rpc(get_in_snd_device(mDevices), 0, mHardware->mMicMute ? 1 : 0);
            }
        }
    }
    return NO_ERROR;
}

String8 AudioHardware::AudioStreamInQ5V2::getParameters(const String8& keys)
{
    AudioParameter param = AudioParameter(keys);
    return param.toString();
}

extern "C" AudioHardwareInterface* createAudioHardware(void) {
    return new AudioHardware();
}

}; // namespace android
