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
    mInit(false), mMicMute(true), mOutput(0)
{
    mInit = true;
}

AudioHardware::~AudioHardware()
{
    closeOutputStream((AudioStreamOut*)mOutput);
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
    return 0;
}

void AudioHardware::closeInputStream(AudioStreamIn* in) {
}

status_t AudioHardware::setMode(int mode)
{
    return NO_ERROR;
}

status_t AudioHardware::setMicMute(bool state)
{
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
    return 4096;
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

extern "C" AudioHardwareInterface* createAudioHardware(void) {
    return new AudioHardware();
}

}; // namespace android
