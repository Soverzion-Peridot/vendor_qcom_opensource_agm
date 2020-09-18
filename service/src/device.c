/*
** Copyright (c) 2019, The Linux Foundation. All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are
** met:
**   * Redistributions of source code must retain the above copyright
**     notice, this list of conditions and the following disclaimer.
**   * Redistributions in binary form must reproduce the above
**     copyright notice, this list of conditions and the following
**     disclaimer in the documentation and/or other materials provided
**     with the distribution.
**   * Neither the name of The Linux Foundation nor the names of its
**     contributors may be used to endorse or promote products derived
**     from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
** WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
** MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
** ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
** BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
** CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
** SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
** BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
** WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
** OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
** IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**/

#define LOG_TAG "AGM: device"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <stdbool.h>
#include "device.h"
#include "metadata.h"
#include "utils.h"
#include <tinyalsa/asoundlib.h>

#define PCM_DEVICE_FILE "/proc/asound/pcm"
#define MAX_RETRY 20 /*Device will try these many times before return an error*/
#define RETRY_INTERVAL 5 /*Retry interval in seconds*/

#ifdef DYNAMIC_LOG_ENABLED
#include <log_xml_parser.h>
#define LOG_MASK AGM_MOD_FILE_DEVICE
#include <log_utils.h>
#endif

#define TRUE 1
#define FALSE 0

#define DEVICE_ENABLE 1
#define DEVICE_DISABLE 0

/* Global list to store supported devices */
struct device_obj **device_list;
static uint32_t num_audio_intfs;

static struct mixer *mixer = NULL;
#define SYSFS_FD_PATH "/sys/kernel/aud_dev/state"
static int sysfs_fd = -1;

#define MAX_BUF_SIZE                 2048
/**
  * The maximum period bytes for dummy dai is 8192 bytes.
  * Hard coding of period size to 960 frames was leading
  * to bigger period bytes for multi channels.
  * So, now based on frame size, Period size is being calculated.
  * 1 frame = bytes_per_sample * channels
  * period size = 8192/(bytes_per_sample * channels)
  */
#define MAX_PERIOD_BUFFER            8192
#define DEFAULT_PERIOD_COUNT         2

#define MAX_USR_INPUT 9

static void update_sysfs_fd (int8_t pcm_id, int8_t state)
{
    char buf[MAX_USR_INPUT]={0};
    snprintf(buf, MAX_USR_INPUT,"%d %d", pcm_id, state);
    if (sysfs_fd >= 0)
        write(sysfs_fd, buf, MAX_USR_INPUT);
    else {
        /*AGM service and sysfs file creation are async events.
         *Also when the syfs node is first created the default
         *user attribute for the sysfs file is root.
         *To change the same we need to execute a command from
         *init scripts, which again are async and there is no
         *deterministic way of scheduling this command only after
         *the sysfs node is created. And all this should happen before
         *we try to open the file from agm context, otherwise the open
         *call would fail with permission denied error.
         *Hence we try to open the file first time when we access it instead
         *of doing it from agm_init.
         *This gives the system enough time for the file attributes to
         *be changed.
         */
        sysfs_fd = open(SYSFS_FD_PATH, O_WRONLY);
        if (sysfs_fd >= 0) {
            write(sysfs_fd, buf, MAX_USR_INPUT);
        } else {
            AGM_LOGE("invalid file handle\n");
        }
    }
}

enum pcm_format agm_to_pcm_format(enum agm_media_format format)
{
    switch (format) {
    case AGM_FORMAT_PCM_S32_LE:
        return PCM_FORMAT_S32_LE;
    case AGM_FORMAT_PCM_S8:
        return PCM_FORMAT_S8;
    case AGM_FORMAT_PCM_S24_3LE:
        return PCM_FORMAT_S24_3LE;
    case AGM_FORMAT_PCM_S24_LE:
        return PCM_FORMAT_S24_LE;
    default:
    case AGM_FORMAT_PCM_S16_LE:
        return PCM_FORMAT_S16_LE;
    };
}

int device_get_snd_card_id()
{
    struct device_obj *dev_obj = device_list[0];

    if (dev_obj == NULL) {
        AGM_LOGE("Invalid device object\n");
        return -EINVAL;
    }
    return dev_obj->card_id;
}

int device_open(struct device_obj *dev_obj)
{
    int ret = 0;
    struct pcm *pcm = NULL;
    struct pcm_config config;

    if (dev_obj == NULL) {
        AGM_LOGE("Invalid device object\n");
        return -EINVAL;
    }

    pthread_mutex_lock(&dev_obj->lock);
    if (dev_obj->refcnt.open) {
        AGM_LOGI("PCM device %u already opened\n",
                 dev_obj->pcm_id);
        dev_obj->refcnt.open++;
        goto done;
    }

    memset(&config, 0, sizeof(struct pcm_config));
    config.channels = dev_obj->media_config.channels;
    config.rate = dev_obj->media_config.rate;
    config.format = agm_to_pcm_format(dev_obj->media_config.format);
    config.period_size = (MAX_PERIOD_BUFFER)/((config.channels) *
                              (get_pcm_bit_width(dev_obj->media_config.format)/8));
    config.period_count = DEFAULT_PERIOD_COUNT;
    config.start_threshold = config.period_size / 4;
    config.stop_threshold = INT_MAX;

    pcm = pcm_open(dev_obj->card_id, dev_obj->pcm_id, dev_obj->pcm_flags,
                &config);
    if (!pcm || !pcm_is_ready(pcm)) {
        AGM_LOGE("Unable to open PCM device %u (%s) rate %u ch %d fmt %u",
                dev_obj->pcm_id, pcm_get_error(pcm), config.rate,
                config.channels, config.format);
        AGM_LOGE("Period Size %d \n", config.period_size);
        ret = -EIO;
        goto done;
    }
    update_sysfs_fd(dev_obj->pcm_id, DEVICE_ENABLE);
    dev_obj->pcm = pcm;
    dev_obj->state = DEV_OPENED;
    dev_obj->refcnt.open++;
done:
    pthread_mutex_unlock(&dev_obj->lock);
    return ret;
}

int device_prepare(struct device_obj *dev_obj)
{
    int ret = 0;

    if (dev_obj == NULL) {
        AGM_LOGE("Invalid device object\n");
        return -EINVAL;
    }

    pthread_mutex_lock(&dev_obj->lock);
    if (dev_obj->refcnt.prepare) {
        AGM_LOGD("PCM device %u already in prepare state\n",
              dev_obj->pcm_id);
        dev_obj->refcnt.prepare++;
        pthread_mutex_unlock(&dev_obj->lock);
        return ret;
    }
    ret = pcm_prepare(dev_obj->pcm);
    if (ret) {
        AGM_LOGE("PCM device %u prepare failed, ret = %d\n",
              dev_obj->pcm_id, ret);
        goto done;
    }

    dev_obj->state = DEV_PREPARED;
    dev_obj->refcnt.prepare++;

done:
    pthread_mutex_unlock(&dev_obj->lock);
    return ret;
}

int device_start(struct device_obj *dev_obj)
{
    int ret = 0;

    if (dev_obj == NULL) {
        AGM_LOGE("Invalid device object\n");
        return -EINVAL;
    }

    pthread_mutex_lock(&dev_obj->lock);
    if (dev_obj->state < DEV_PREPARED) {
            AGM_LOGE("PCM device %u not yet prepared, exiting\n",
                  dev_obj->pcm_id);
            ret =  -1;
            goto done;
    }

    if (dev_obj->refcnt.start) {
        AGM_LOGI("PCM device %u already in start state\n",
              dev_obj->pcm_id);
        dev_obj->refcnt.start++;
        goto done;
    }

    dev_obj->state = DEV_STARTED;
    dev_obj->refcnt.start++;

done:
    pthread_mutex_unlock(&dev_obj->lock);
    return ret;
}

int device_stop(struct device_obj *dev_obj)
{
    int ret = 0;

    if(dev_obj == NULL) {
        AGM_LOGE("Invalid device object\n");
        return -EINVAL;
    }

    pthread_mutex_lock(&dev_obj->lock);
    if (!dev_obj->refcnt.start) {
        AGM_LOGE("PCM device %u already stopped\n",
              dev_obj->pcm_id);
        goto done;
    }

    dev_obj->refcnt.start--;
    if (dev_obj->refcnt.start == 0) {
        ret = pcm_stop(dev_obj->pcm);
        if (ret) {
            AGM_LOGE("PCM device %u stop failed, ret = %d\n",
                    dev_obj->pcm_id, ret);
        }
        dev_obj->state = DEV_STOPPED;
    }

done:
    pthread_mutex_unlock(&dev_obj->lock);
    return ret;
}

int device_close(struct device_obj *dev_obj)
{
    int ret = 0;

    if (dev_obj == NULL) {
        AGM_LOGE("Invalid device object\n");
        return -EINVAL;
    }

    pthread_mutex_lock(&dev_obj->lock);
    if (--dev_obj->refcnt.open == 0) {
        update_sysfs_fd(dev_obj->pcm_id, DEVICE_DISABLE);
        ret = pcm_close(dev_obj->pcm);
        if (ret) {
            AGM_LOGE("PCM device %u close failed, ret = %d\n",
                     dev_obj->pcm_id, ret);
        }
        dev_obj->state = DEV_CLOSED;
        dev_obj->refcnt.prepare = 0;
        dev_obj->refcnt.start = 0;
    }

    pthread_mutex_unlock(&dev_obj->lock);
    return ret;
}

enum device_state device_current_state(struct device_obj *dev_obj)
{
    return dev_obj->state;
}

int device_get_aif_info_list(struct aif_info *aif_list, size_t *audio_intfs)
{
    struct device_obj *dev_obj;
    uint32_t copied = 0;
    uint32_t requested = *audio_intfs;

    if (*audio_intfs == 0){
        *audio_intfs = num_audio_intfs;
    } else {
        for(copied = 0; (copied < num_audio_intfs) && (copied < requested);
                                                               copied++) {
            dev_obj = device_list[copied];
            strlcpy(aif_list[copied].aif_name, dev_obj->name,
                                          AIF_NAME_MAX_LEN );
            aif_list[copied].dir = dev_obj->hw_ep_info.dir;
        }
        *audio_intfs = copied;
    }
    return 0;
}

int device_get_obj(uint32_t device_idx, struct device_obj **dev_obj)
{
    if (device_idx > num_audio_intfs) {
        AGM_LOGE("Invalid device_id %u, max_supported device id: %d\n",
                device_idx, num_audio_intfs);
        return -EINVAL;
    }

    *dev_obj = device_list[device_idx];
    return 0;
}

int device_set_media_config(struct device_obj *dev_obj,
          struct agm_media_config *device_media_config)
{
   if (dev_obj == NULL || device_media_config == NULL) {
       AGM_LOGE("Invalid device object\n");
       return -EINVAL;
   }
   dev_obj->media_config.channels = device_media_config->channels;
   dev_obj->media_config.rate = device_media_config->rate;
   dev_obj->media_config.format = device_media_config->format;
   dev_obj->media_config.data_format = device_media_config->data_format;

   return 0;
}

int device_set_metadata(struct device_obj *dev_obj, uint32_t size,
                                                uint8_t *metadata)
{
   metadata_free(&dev_obj->metadata);
   return metadata_copy(&(dev_obj->metadata), size, metadata);
}

int device_set_params(struct device_obj *dev_obj,
                      void *payload, size_t size)
{
   int ret = 0;

   pthread_mutex_unlock(&dev_obj->lock);

   if (dev_obj->params) {
       free(dev_obj->params);
       dev_obj->params = NULL;
       dev_obj->params_size = 0;
   }

   dev_obj->params = calloc(1, size);
   if (!dev_obj->params) {
       AGM_LOGE("No memory for dev params on dev_id:%d\n",
                                   dev_obj->pcm_id);
       ret = -EINVAL;
       goto done;
   }

   memcpy(dev_obj->params, payload, size);
   dev_obj->params_size = size;

done:
   pthread_mutex_unlock(&dev_obj->lock);
   return ret;
}

int device_get_channel_map(struct device_obj *dev_obj, uint32_t **chmap)
{
    struct mixer_ctl *ctl = NULL;
    char *mixer_str = NULL;
    int ctl_len = 0, ret = 0, card_id;
    void *payload = NULL;

    if (mixer == NULL) {
        card_id = device_get_snd_card_id();
        if (card_id < 0) {
            AGM_LOGE("failed to get card_id");
            return -EINVAL;
        }

        mixer = mixer_open(card_id);
        if (!mixer) {
            AGM_LOGE("failed to get mixer handle");
            return -EINVAL;
        }
    }

    ctl_len = strlen(dev_obj->name) + 1 + strlen("Channel Map") + 1;
    mixer_str = calloc(1, ctl_len);
    if (!mixer_str) {
        AGM_LOGE("Failed to allocate memory for mixer_str");
        return -ENOMEM;
    }

    payload = calloc(16, sizeof(uint32_t));
    if (!payload) {
        AGM_LOGE("Failed to allocate memory for payload");
        ret = -ENOMEM;
        goto done;
    }

    snprintf(mixer_str, ctl_len, "%s %s", dev_obj->name, "Channel Map");

    ctl = mixer_get_ctl_by_name(mixer, mixer_str);
    if (!ctl) {
        printf("Invalid mixer control: %s\n", mixer_str);
        free(mixer_str);
        ret = -ENOENT;
        goto err_get_ctl;
    }

    ret = mixer_ctl_get_array(ctl, payload, 16 * sizeof(uint32_t));
    if (ret < 0) {
        printf("Failed to mixer_ctl_get_array\n");
        goto err_get_ctl;
    }
    *chmap = payload;
    goto done;

err_get_ctl:
    free(payload);
done:
    free(mixer_str);
    return ret;
}

int parse_snd_card()
{
    char buffer[MAX_BUF_SIZE];
    unsigned int count = 0, i = 0;
    FILE *fp;
    int ret = 0;

    fp = fopen(PCM_DEVICE_FILE, "r");
    if (!fp) {
        AGM_LOGE("ERROR. %s file open failed\n",
                         PCM_DEVICE_FILE);
        return -ENODEV;
    }

    while (fgets(buffer, MAX_BUF_SIZE - 1, fp) != NULL)
        num_audio_intfs++;

    device_list = calloc (num_audio_intfs, sizeof(struct device_obj*));
    if (!device_list) {
        ret = -ENOMEM;
        goto close_file;
    }

    rewind(fp);

    while (fgets(buffer, MAX_BUF_SIZE - 1, fp) != NULL)
    {
        struct device_obj *dev_obj = calloc(1, sizeof(struct device_obj));

        if (!dev_obj) {
            AGM_LOGE("failed to allocate device_obj mem\n");
            ret = -ENOMEM;
            goto free_device;
        }

        AGM_LOGV("buffer: %s\n", buffer);
        /* For Non-DPCM Dai-links, it is in the format of:
         * <card_num>-<pcm_device_id>: <pcm->idname> : <pcm->name> :
                                                <playback/capture> 1
         * Here, pcm->idname is in the form of "<dai_link->stream_name>
                                          <codec_name>-<num_codecs>"
         */
        sscanf(buffer, "%02u-%02u: %80s", &dev_obj->card_id,
                           &dev_obj->pcm_id, dev_obj->name);
        AGM_LOGD("%d:%d:%s\n", dev_obj->card_id, dev_obj->pcm_id, dev_obj->name);

        if (strstr(buffer, "playback"))
            dev_obj->pcm_flags = PCM_OUT;
        else
            dev_obj->pcm_flags = PCM_IN;

        /* populate the hw_ep_ifo for all the available pcm-id's*/
        ret = populate_device_hw_ep_info(dev_obj);
        if (ret) {
           AGM_LOGE("hw_ep_info parsing failed %s\n",
                                dev_obj->name);
           free(dev_obj);
           ret = 0;
           continue;
        }

        pthread_mutex_init(&dev_obj->lock, (const pthread_mutexattr_t *) NULL);
        device_list[count] = dev_obj;
        count++;
        dev_obj = NULL;
    }
    /*
     * count 0 indicates that expected sound card was not registered
     * inform the upper layers to try again after sometime.
     */
    if (count == 0) {
        ret = -EAGAIN;
        goto free_device;
    }

    num_audio_intfs = count;
    goto close_file;

free_device:
    for (i = 0; i < num_audio_intfs; i++) {
        if (device_list[i])
            free(device_list[i]);
    }
    if (device_list)
       free(device_list);
close_file:
    fclose(fp);
    return ret;
}

int device_init()
{
    int ret = 0;
    uint32_t retries = MAX_RETRY;

    do {
       ret = parse_snd_card();
       if (ret == -EAGAIN) {
           AGM_LOGE("no valid snd device found retry %d times\n", retries);
           retries--;
           sleep(RETRY_INTERVAL);
       } else
           break;
    } while ( retries > 0);

    return ret;
}

void device_deinit()
{
    unsigned int list_count = 0;
    struct device_obj *dev_obj = NULL;

    AGM_LOGE("%s:device deinit called\n");
    for (list_count = 0; list_count < num_audio_intfs; list_count++) {
        dev_obj = device_list[list_count];
        metadata_free(&dev_obj->metadata);

        if (dev_obj->params)
            free(dev_obj->params);

        free(dev_obj);
        dev_obj = NULL;
    }
    if (sysfs_fd >= 0)
        close(sysfs_fd);
    sysfs_fd = -1;

    if (mixer)
        mixer_close(mixer);

    free(device_list);
    device_list = NULL;
}
