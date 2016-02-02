/******************************************************************************
 *
 *  Copyright (C) 2009-2012 Broadcom Corporation
 *  Copyright (c) 2013, Linux Foundation. All rights reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/******************************************************************************
 **
 **  Name:          btif_media_task.c
 **
 **  Description:   This is the multimedia module for the BTIF system.  It
 **                 contains task implementations AV, HS and HF profiles
 **                 audio & video processing
 **
 ******************************************************************************/
//#define BT_AUDIO_SYSTRACE_LOG
#ifdef BT_AUDIO_SYSTRACE_LOG
#define ATRACE_TAG ATRACE_TAG_ALWAYS
#endif

#define LOG_TAG "bt_btif_media"

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>
#include <errno.h>

#include "bt_target.h"
#include "osi/include/fixed_queue.h"
#include "gki.h"
#include "bta_api.h"
#include "btu.h"
#include "bta_sys.h"
#include "bta_sys_int.h"

#include "bta_av_api.h"
#include "a2d_api.h"
#include "a2d_sbc.h"
#include "a2d_int.h"
#include "bta_av_sbc.h"
#include "bta_av_ci.h"
#include "l2c_api.h"

#include "btif_av_co.h"
#include "btif_media.h"

#include "osi/include/alarm.h"
#include "osi/include/log.h"
#include "osi/include/thread.h"

#if (BTA_AV_INCLUDED == TRUE)
#include "sbc_encoder.h"
#endif

#include <hardware/bluetooth.h>
#include "audio_a2dp_hw.h"
#include "btif_av.h"
#include "btif_sm.h"
#include "btif_util.h"
#if (BTA_AV_SINK_INCLUDED == TRUE)
#include "oi_codec_sbc.h"
#include "oi_status.h"
#endif
#ifdef USE_AUDIO_TRACK
#include "bluetoothTrack.h"
#endif
#include "stdio.h"
#include <dlfcn.h>

#if (BTA_AV_SINK_INCLUDED == TRUE)
OI_CODEC_SBC_DECODER_CONTEXT context;
OI_UINT32 contextData[CODEC_DATA_WORDS(2, SBC_CODEC_FAST_FILTER_BUFFERS)];
OI_INT16 pcmData[15*SBC_MAX_SAMPLES_PER_FRAME*SBC_MAX_CHANNELS];
#endif

#ifdef BT_AUDIO_SYSTRACE_LOG
#include <cutils/trace.h>
#define PERF_SYSTRACE 1
#endif

#ifdef BTA_AV_SPLIT_A2DP_ENABLED
#include "bta_api.h"
#endif


/*****************************************************************************
 **  Constants
 *****************************************************************************/

#ifndef AUDIO_CHANNEL_OUT_MONO
#define AUDIO_CHANNEL_OUT_MONO 0x01
#endif

#ifndef AUDIO_CHANNEL_OUT_STEREO
#define AUDIO_CHANNEL_OUT_STEREO 0x03
#endif

/* BTIF media cmd event definition : BTIF_MEDIA_TASK_CMD */
enum
{
    BTIF_MEDIA_START_AA_TX = 1,
    BTIF_MEDIA_STOP_AA_TX,
    BTIF_MEDIA_AA_RX_RDY,
    BTIF_MEDIA_UIPC_RX_RDY,
    BTIF_MEDIA_SBC_ENC_INIT,
    BTIF_MEDIA_SBC_ENC_UPDATE,
    BTIF_MEDIA_SBC_DEC_INIT,
    BTIF_MEDIA_VIDEO_DEC_INIT,
    BTIF_MEDIA_FLUSH_AA_TX,
    BTIF_MEDIA_FLUSH_AA_RX,
    BTIF_MEDIA_AUDIO_FEEDING_INIT,
    BTIF_MEDIA_AUDIO_RECEIVING_INIT,
    BTIF_MEDIA_AUDIO_SINK_CFG_UPDATE,
    BTIF_MEDIA_AUDIO_SINK_CLEAR_TRACK
#ifdef USE_AUDIO_TRACK
    ,BTIF_MEDIA_AUDIO_SINK_SET_FOCUS_STATE
#endif
#ifdef BTA_AV_SPLIT_A2DP_ENABLED
    ,BTIF_MEDIA_START_VS_CMD,
    BTIF_MEDIA_STOP_VS_CMD,
    BTIF_MEDIA_RESET_VS_STATE,
    BTIF_MEDIA_VS_A2DP_START_SUCCESS,
    BTIF_MEDIA_VS_A2DP_STOP_SUCCESS,
    BTIF_MEDIA_VS_A2DP_MEDIA_CHNL_CFG_SUCCESS,
    BTIF_MEDIA_VS_A2DP_WRITE_SBC_CFG_SUCCESS,
    BTIF_MEDIA_VS_A2DP_PREF_BIT_RATE_SUCCESS,
    BTIF_MEDIA_VS_A2DP_SET_SCMST_HDR_SUCCESS,
    BTIF_MEDIA_VS_A2DP_STOP_FAILURE
#endif
};

enum {
    MEDIA_TASK_STATE_OFF = 0,
    MEDIA_TASK_STATE_ON = 1,
    MEDIA_TASK_STATE_SHUTTING_DOWN = 2
};

/* Macro to multiply the media task tick */
#ifndef BTIF_MEDIA_NUM_TICK
#define BTIF_MEDIA_NUM_TICK      1
#endif

/* Media task tick in milliseconds, must be set to multiple of
   (1000/TICKS_PER_SEC) (10) */

#define BTIF_MEDIA_TIME_TICK                     (20 * BTIF_MEDIA_NUM_TICK)
#define A2DP_DATA_READ_POLL_MS    (BTIF_MEDIA_TIME_TICK / 2)
#define BTIF_SINK_MEDIA_TIME_TICK                (20 * BTIF_MEDIA_NUM_TICK)


/* buffer pool */
#define BTIF_MEDIA_AA_POOL_ID GKI_POOL_ID_3
#define BTIF_MEDIA_AA_BUF_SIZE GKI_BUF3_SIZE

/* offset */
#if (BTA_AV_CO_CP_SCMS_T == TRUE)
#define BTIF_MEDIA_AA_SBC_OFFSET (AVDT_MEDIA_OFFSET + BTA_AV_SBC_HDR_SIZE + 1)
#else
#define BTIF_MEDIA_AA_SBC_OFFSET (AVDT_MEDIA_OFFSET + BTA_AV_SBC_HDR_SIZE)
#endif

/* Define the bitrate step when trying to match bitpool value */
#ifndef BTIF_MEDIA_BITRATE_STEP
#define BTIF_MEDIA_BITRATE_STEP 5
#endif

#if defined(SAMPLE_RATE_48K) || defined(BTA_AV_SPLIT_A2DP_DEF_FREQ_48KHZ)
#define DEFAULT_SBC_BITRATE 345

#ifndef BTIF_A2DP_NON_EDR_MAX_RATE
#define BTIF_A2DP_NON_EDR_MAX_RATE 237
#endif
#else
#define DEFAULT_SBC_BITRATE 328

#ifndef BTIF_A2DP_NON_EDR_MAX_RATE
#define BTIF_A2DP_NON_EDR_MAX_RATE 229
#endif
#endif

#if (BTA_AV_CO_CP_SCMS_T == TRUE)
/* A2DP header will contain a CP header of size 1 */
#define A2DP_HDR_SIZE               2
#else
#define A2DP_HDR_SIZE               1
#endif
#define MAX_SBC_HQ_FRAME_SIZE_44_1  119
#define MAX_SBC_HQ_FRAME_SIZE_48    115

/* 2DH5 payload size (679 bytes) - (4 bytes L2CAP Header + 12 bytes AVDTP Header) */
#define MAX_2MBPS_AVDTP_MTU         663

#define USEC_PER_SEC 1000000L
#define TPUT_STATS_INTERVAL_US (3000*1000)

/*
 * CONGESTION COMPENSATION CTRL ::
 *
 * Thus setting controls how many buffers we will hold in media task
 * during temp link congestion. Together with the stack buffer queues
 * it controls much temporary a2dp link congestion we can
 * compensate for. It however also depends on the default run level of sinks
 * jitterbuffers. Depending on type of sink this would vary.
 * Ideally the (SRC) max tx buffer capacity should equal the sinks
 * jitterbuffer runlevel including any intermediate buffers on the way
 * towards the sinks codec.
 */

/* fixme -- define this in pcm time instead of buffer count */

/* The typical runlevel of the tx queue size is ~1 buffer
   but due to link flow control or thread preemption in lower
   layers we might need to temporarily buffer up data */
/* 18 frames is equivalent to 6.89*18*2.9 ~= 360 ms @ 44.1 khz, 20 ms mediatick */
#define MAX_OUTPUT_A2DP_FRAME_QUEUE_SZ 10
#ifndef MAX_PCM_FRAME_NUM_PER_TICK
#ifdef SAMPLE_RATE_48K
/* If a frame is 512 bytes and a tick is 3840 bytes (48K) then allow up to
 * two full ticks to be sent per tick which is 9680 / 512 = 18
 */
#define MAX_PCM_FRAME_NUM_PER_TICK     18
#else
#define MAX_PCM_FRAME_NUM_PER_TICK     14
#endif
#endif
#define MAX_PCM_ITER_NUM_PER_TICK     3

/* In case of A2DP SINK, we will delay start by 5 AVDTP Packets*/
#define MAX_A2DP_DELAYED_START_FRAME_COUNT 3
#define PACKET_PLAYED_PER_TICK_48 8
#define PACKET_PLAYED_PER_TICK_44 7
#define PACKET_PLAYED_PER_TICK_32 5
#define PACKET_PLAYED_PER_TICK_16 3

typedef struct
{
    UINT16 num_frames_to_be_processed;
    UINT16 len;
    UINT16 offset;
    UINT16 layer_specific;
} tBT_SBC_HDR;

typedef struct
{
    UINT32 aa_frame_counter;
    INT32  aa_feed_counter;
    INT32  aa_feed_residue;
    UINT32 counter;
    UINT32 bytes_per_tick;  /* pcm bytes read each media task tick */
} tBTIF_AV_MEDIA_FEEDINGS_PCM_STATE;

typedef union
{
    tBTIF_AV_MEDIA_FEEDINGS_PCM_STATE pcm;
} tBTIF_AV_MEDIA_FEEDINGS_STATE;

typedef struct
{
#if (BTA_AV_INCLUDED == TRUE)
    BUFFER_Q TxAaQ;
    BUFFER_Q RxSbcQ;
    BOOLEAN is_tx_timer;
    BOOLEAN is_rx_timer;
    UINT16 TxAaMtuSize;
    UINT32 timestamp;
    UINT8 TxTranscoding;
    tBTIF_AV_FEEDING_MODE feeding_mode;
    tBTIF_AV_MEDIA_FEEDINGS media_feeding;
    tBTIF_AV_MEDIA_FEEDINGS_STATE media_feeding_state;
    SBC_ENC_PARAMS encoder;
    UINT8 busy_level;
    void* av_sm_hdl;
    UINT8 a2dp_cmd_pending; /* we can have max one command pending */
    BOOLEAN tx_flush; /* discards any outgoing data when true */
    BOOLEAN rx_flush; /* discards any incoming data when true */
    UINT8 peer_sep;
    BOOLEAN data_channel_open;
    UINT8   frames_to_process;

    UINT32  sample_rate;
    UINT8   channel_count;
#ifdef USE_AUDIO_TRACK
    btif_media_audio_focus_state rx_audio_focus_state;
#endif
    alarm_t *media_alarm;
    alarm_t *decode_alarm;
    UINT8 TxNumSBCFrames;
#ifdef BTA_AV_SPLIT_A2DP_ENABLED
    UINT8 max_bitpool;
    UINT8 min_bitpool;
    BOOLEAN vs_configs_exchanged;
    BOOLEAN tx_started;
    BOOLEAN tx_stop_initiated;
#endif

#endif

} tBTIF_MEDIA_CB;

typedef struct {
    long long rx;
    long long rx_tot;
    long long tx;
    long long tx_tot;
    long long ts_prev_us;
} t_stat;

static UINT64 last_frame_us = 0;

static void btif_a2dp_data_cb(tUIPC_CH_ID ch_id, tUIPC_EVENT event);
static void btif_a2dp_ctrl_cb(tUIPC_CH_ID ch_id, tUIPC_EVENT event);
static void btif_a2dp_encoder_update(void);
#if (BTA_AV_SINK_INCLUDED == TRUE)
extern OI_STATUS OI_CODEC_SBC_DecodeFrame(OI_CODEC_SBC_DECODER_CONTEXT *context,
                                          const OI_BYTE **frameData,
                                          unsigned long *frameBytes,
                                          OI_INT16 *pcmData,
                                          unsigned long *pcmBytes);
extern OI_STATUS OI_CODEC_SBC_DecoderReset(OI_CODEC_SBC_DECODER_CONTEXT *context,
                                           unsigned long *decoderData,
                                           unsigned long decoderDataBytes,
                                           OI_UINT8 maxChannels,
                                           OI_UINT8 pcmStride,
                                           OI_BOOL enhanced);
#endif
static void btif_media_flush_q(BUFFER_Q *p_q);
static void btif_media_task_aa_handle_stop_decoding(void );
static void btif_media_task_aa_rx_flush(void);

static UINT8 check_for_max_number_of_frames_per_packet();
static const char *dump_media_event(UINT16 event);
static void btif_media_thread_init(void *context);
static void btif_media_thread_cleanup(void *context);
static void btif_media_thread_handle_cmd(fixed_queue_t *queue, void *context);

/* Handle incoming media packets A2DP SINK streaming*/
#if (BTA_AV_SINK_INCLUDED == TRUE)
static void btif_media_task_handle_inc_media(tBT_SBC_HDR*p_msg);
#endif

#if (BTA_AV_INCLUDED == TRUE)
static void btif_media_send_aa_frame(void);
static void btif_media_task_feeding_state_reset(void);
static void btif_media_task_aa_start_tx(void);
static void btif_media_task_aa_stop_tx(void);
static void btif_media_task_enc_init(BT_HDR *p_msg);
static void btif_media_task_enc_update(BT_HDR *p_msg);
static void btif_media_task_audio_feeding_init(BT_HDR *p_msg);
static void btif_media_task_aa_tx_flush(BT_HDR *p_msg);
static void btif_media_aa_prep_2_send(UINT8 nb_frame);
#if (BTA_AV_SINK_INCLUDED == TRUE)
static void btif_media_task_aa_handle_decoder_reset(BT_HDR *p_msg);
static void btif_media_task_aa_handle_clear_track(void);
#endif
static void btif_media_task_aa_handle_start_decoding(void);
#endif
BOOLEAN btif_media_task_clear_track(void);

static void btif_media_task_aa_handle_timer(UNUSED_ATTR void *context);
static void btif_media_task_avk_handle_timer(UNUSED_ATTR void *context);
extern BOOLEAN btif_hf_is_call_idle();

#ifdef BTA_AV_SPLIT_A2DP_ENABLED
void btif_media_send_reset_vendor_state();
void btif_media_on_start_vendor_command();
void btif_media_start_vendor_command();
void btif_media_on_stop_vendor_command();
BOOLEAN btif_media_send_vendor_pref_bit_rate();
BOOLEAN btif_media_send_vendor_write_sbc_cfg();
BOOLEAN btif_media_send_vendor_media_chn_cfg();
BOOLEAN btif_media_send_vendor_stop();
BOOLEAN btif_media_send_vendor_start();
#endif


static tBTIF_MEDIA_CB btif_media_cb;
static int media_task_running = MEDIA_TASK_STATE_OFF;

static fixed_queue_t *btif_media_cmd_msg_queue;
static thread_t *worker_thread;

/*****************************************************************************
 **  Misc helper functions
 *****************************************************************************/

static UINT64 time_now_us()
{
    struct timespec ts_now;
    clock_gettime(CLOCK_BOOTTIME, &ts_now);
    return ((UINT64)ts_now.tv_sec * USEC_PER_SEC) + ((UINT64)ts_now.tv_nsec / 1000);
}

static void log_tstamps_us(char *comment)
{
    #define USEC_PER_MSEC 1000L
    static UINT64 prev_us = 0;
    const UINT64 now_us = time_now_us();
    static UINT64 diff_us = 0;

    diff_us = now_us - prev_us;
    if ((diff_us / USEC_PER_MSEC) > (BTIF_MEDIA_TIME_TICK + 10))
    {
        APPL_TRACE_ERROR("[%s] ts %08llu, diff : %08llu, queue sz %d", comment, now_us, diff_us,
                GKI_queue_length(&btif_media_cb.TxAaQ));
    }
    else
    {
        APPL_TRACE_DEBUG("[%s] ts %08llu, diff : %08llu, queue sz %d", comment, now_us, diff_us,
                GKI_queue_length(&btif_media_cb.TxAaQ));
    }

    prev_us = now_us;
}

UNUSED_ATTR static const char *dump_media_event(UINT16 event)
{
    switch(event)
    {
        CASE_RETURN_STR(BTIF_MEDIA_START_AA_TX)
        CASE_RETURN_STR(BTIF_MEDIA_STOP_AA_TX)
        CASE_RETURN_STR(BTIF_MEDIA_AA_RX_RDY)
        CASE_RETURN_STR(BTIF_MEDIA_UIPC_RX_RDY)
        CASE_RETURN_STR(BTIF_MEDIA_SBC_ENC_INIT)
        CASE_RETURN_STR(BTIF_MEDIA_SBC_ENC_UPDATE)
        CASE_RETURN_STR(BTIF_MEDIA_SBC_DEC_INIT)
        CASE_RETURN_STR(BTIF_MEDIA_VIDEO_DEC_INIT)
        CASE_RETURN_STR(BTIF_MEDIA_FLUSH_AA_TX)
        CASE_RETURN_STR(BTIF_MEDIA_FLUSH_AA_RX)
        CASE_RETURN_STR(BTIF_MEDIA_AUDIO_FEEDING_INIT)
        CASE_RETURN_STR(BTIF_MEDIA_AUDIO_RECEIVING_INIT)
        CASE_RETURN_STR(BTIF_MEDIA_AUDIO_SINK_CFG_UPDATE)
        CASE_RETURN_STR(BTIF_MEDIA_AUDIO_SINK_CLEAR_TRACK)
#ifdef USE_AUDIO_TRACK
        CASE_RETURN_STR(BTIF_MEDIA_AUDIO_SINK_SET_FOCUS_STATE)
#endif
        default:
            return "UNKNOWN MEDIA EVENT";
    }
}

/*****************************************************************************
 **  A2DP CTRL PATH
 *****************************************************************************/

static const char* dump_a2dp_ctrl_event(UINT8 event)
{
    switch(event)
    {
        CASE_RETURN_STR(A2DP_CTRL_CMD_NONE)
        CASE_RETURN_STR(A2DP_CTRL_CMD_CHECK_READY)
        CASE_RETURN_STR(A2DP_CTRL_CMD_START)
        CASE_RETURN_STR(A2DP_CTRL_CMD_STOP)
        CASE_RETURN_STR(A2DP_CTRL_CMD_SUSPEND)
        default:
            return "UNKNOWN MSG ID";
    }
}

static void btif_audiopath_detached(void)
{
    APPL_TRACE_IMP("## AUDIO PATH DETACHED ##");

    /*  send stop request only if we are actively streaming and haven't received
        a stop request. Potentially audioflinger detached abnormally */
    if (btif_media_cb.is_tx_timer)
    {
        /* post stop event and wait for audio path to stop */
        btif_dispatch_sm_event(BTIF_AV_STOP_STREAM_REQ_EVT, NULL, 0);
    }
}

static void a2dp_cmd_acknowledge(int status)
{
    UINT8 ack = status;

    APPL_TRACE_IMP("## a2dp ack : %s, status %d ##",
          dump_a2dp_ctrl_event(btif_media_cb.a2dp_cmd_pending), status);

    /* sanity check */
    if (btif_media_cb.a2dp_cmd_pending == A2DP_CTRL_CMD_NONE)
    {
        APPL_TRACE_ERROR("warning : no command pending, ignore ack");
        return;
    }

    /* clear pending */
    btif_media_cb.a2dp_cmd_pending = A2DP_CTRL_CMD_NONE;

    /* acknowledge start request */
    UIPC_Send(UIPC_CH_ID_AV_CTRL, 0, &ack, 1);
}


static void btif_recv_ctrl_data(void)
{
    UINT8 cmd = 0;
    int n;
    n = UIPC_Read(UIPC_CH_ID_AV_CTRL, NULL, &cmd, 1);

    /* detach on ctrl channel means audioflinger process was terminated */
    if (n == 0)
    {
        APPL_TRACE_IMP("CTRL CH DETACHED");
        UIPC_Close(UIPC_CH_ID_AV_CTRL);
        /* we can operate only on datachannel, if af client wants to
           do send additional commands the ctrl channel would be reestablished */
        //btif_audiopath_detached();
        return;
    }

    APPL_TRACE_IMP("a2dp-ctrl-cmd : %s", dump_a2dp_ctrl_event(cmd));

    btif_media_cb.a2dp_cmd_pending = cmd;

    switch(cmd)
    {
        case A2DP_CTRL_CMD_CHECK_READY:

            if (media_task_running == MEDIA_TASK_STATE_SHUTTING_DOWN)
            {
                a2dp_cmd_acknowledge(A2DP_CTRL_ACK_FAILURE);
                return;
            }

            /* check whether av is ready to setup a2dp datapath */
            if ((btif_av_stream_ready() == TRUE) || (btif_av_stream_started_ready() == TRUE))
            {
                a2dp_cmd_acknowledge(A2DP_CTRL_ACK_SUCCESS);
            }
            else
            {
                a2dp_cmd_acknowledge(A2DP_CTRL_ACK_FAILURE);
            }
            break;

        case A2DP_CTRL_CMD_CHECK_STREAM_STARTED:

            if((btif_av_stream_started_ready() == TRUE))
                a2dp_cmd_acknowledge(A2DP_CTRL_ACK_SUCCESS);
            else
                a2dp_cmd_acknowledge(A2DP_CTRL_ACK_FAILURE);
            break;

        case A2DP_CTRL_CMD_START:
            /* Don't sent START request to stack while we are in call.
               Some headsets like the Sony MW600, don't allow AVDTP START
               in call and respond BAD_STATE. */
            if (!btif_hf_is_call_idle())
            {
                a2dp_cmd_acknowledge(A2DP_CTRL_ACK_INCALL_FAILURE);
                break;
            }
            /* In Dual A2dp, first check for started state of stream
            * as we dont want to START again as while doing Handoff
            * the stack state will be started, so it is not needed
            * to send START again, just open the media socket
            * and ACK the audio HAL.*/
            if (btif_av_stream_started_ready())
            {
#ifndef BTA_AV_SPLIT_A2DP_ENABLED
                /* already started, setup audio data channel listener
                * and ack back immediately */
                UIPC_Open(UIPC_CH_ID_AV_AUDIO, btif_a2dp_data_cb);
#else
                APPL_TRACE_DEBUG("Av stream alreday started");
                if (btif_media_cb.peer_sep == AVDT_TSEP_SNK)
                    btif_a2dp_encoder_update();
#endif
                a2dp_cmd_acknowledge(A2DP_CTRL_ACK_SUCCESS);
            }
            else if (btif_av_stream_ready() == TRUE)
            {
                /* setup audio data channel listener */
#ifndef BTA_AV_SPLIT_A2DP_ENABLED
                    /* already started, setup audio data channel listener
                    * and ack back immediately */
                UIPC_Open(UIPC_CH_ID_AV_AUDIO, btif_a2dp_data_cb);
#else
                APPL_TRACE_DEBUG("Av stream ready");
#endif
                /* post start event and wait for audio path to open */
                btif_dispatch_sm_event(BTIF_AV_START_STREAM_REQ_EVT, NULL, 0);

#if (BTA_AV_SINK_INCLUDED == TRUE)
                if (btif_media_cb.peer_sep == AVDT_TSEP_SRC)
                    a2dp_cmd_acknowledge(A2DP_CTRL_ACK_SUCCESS);
#endif
            }
            else
            {
                a2dp_cmd_acknowledge(A2DP_CTRL_ACK_FAILURE);
                break;
            }
            break;

        case A2DP_CTRL_CMD_STOP:
#ifndef BTA_AV_SPLIT_A2DP_ENABLED
            if (btif_media_cb.peer_sep == AVDT_TSEP_SNK && btif_media_cb.is_tx_timer == FALSE)
#else
            if (btif_media_cb.peer_sep == AVDT_TSEP_SNK && btif_media_cb.tx_started == FALSE)
#endif
            {
                /* we are already stopped, just ack back */
                a2dp_cmd_acknowledge(A2DP_CTRL_ACK_SUCCESS);
                break;
            }

            APPL_TRACE_DEBUG("Stop stream request to Av");
            btif_dispatch_sm_event(BTIF_AV_STOP_STREAM_REQ_EVT, NULL, 0);

            a2dp_cmd_acknowledge(A2DP_CTRL_ACK_SUCCESS);
            break;

        case A2DP_CTRL_CMD_SUSPEND:
            /* local suspend */
            if (btif_av_stream_started_ready())
            {
                APPL_TRACE_DEBUG("Suspend stream request to Av");
                btif_dispatch_sm_event(BTIF_AV_SUSPEND_STREAM_REQ_EVT, NULL, 0);
            }
            else
            {
                /* if we are not in started state, just ack back ok and let
                   audioflinger close the channel. This can happen if we are
                   remotely suspended, clear REMOTE SUSPEND Flag */
                btif_av_clear_remote_suspend_flag();
                a2dp_cmd_acknowledge(A2DP_CTRL_ACK_SUCCESS);
            }
            break;

        case A2DP_CTRL_GET_AUDIO_CONFIG:
        {
            uint32_t sample_rate = btif_media_cb.sample_rate;
            uint8_t channel_count = btif_media_cb.channel_count;

            a2dp_cmd_acknowledge(A2DP_CTRL_ACK_SUCCESS);
            UIPC_Send(UIPC_CH_ID_AV_CTRL, 0, (UINT8 *)&sample_rate, 4);
            UIPC_Send(UIPC_CH_ID_AV_CTRL, 0, &channel_count, 1);
            break;
        }

        default:
            APPL_TRACE_ERROR("UNSUPPORTED CMD (%d)", cmd);
            a2dp_cmd_acknowledge(A2DP_CTRL_ACK_FAILURE);
            break;
    }
    APPL_TRACE_IMP("a2dp-ctrl-cmd : %s DONE", dump_a2dp_ctrl_event(cmd));
}

static void btif_a2dp_ctrl_cb(tUIPC_CH_ID ch_id, tUIPC_EVENT event)
{
    UNUSED(ch_id);

    APPL_TRACE_IMP("A2DP-CTRL-CHANNEL EVENT %s", dump_uipc_event(event));

    switch(event)
    {
        case UIPC_OPEN_EVT:
            /* fetch av statemachine handle */
            btif_media_cb.av_sm_hdl = btif_av_get_sm_handle();
            break;

        case UIPC_CLOSE_EVT:
            /* restart ctrl server unless we are shutting down */
            if (media_task_running == MEDIA_TASK_STATE_ON)
                UIPC_Open(UIPC_CH_ID_AV_CTRL , btif_a2dp_ctrl_cb);
            break;

        case UIPC_RX_DATA_READY_EVT:
            btif_recv_ctrl_data();
            break;

        default :
            APPL_TRACE_ERROR("### A2DP-CTRL-CHANNEL EVENT %d NOT HANDLED ###", event);
            break;
    }
}

static void btif_a2dp_data_cb(tUIPC_CH_ID ch_id, tUIPC_EVENT event)
{
    UNUSED(ch_id);

    APPL_TRACE_DEBUG("BTIF MEDIA (A2DP-DATA) EVENT %s", dump_uipc_event(event));

    switch(event)
    {
        case UIPC_OPEN_EVT:

            /*  read directly from media task from here on (keep callback for
                connection events */
            UIPC_Ioctl(UIPC_CH_ID_AV_AUDIO, UIPC_REG_REMOVE_ACTIVE_READSET, NULL);
            UIPC_Ioctl(UIPC_CH_ID_AV_AUDIO, UIPC_SET_READ_POLL_TMO,
                       (void *)A2DP_DATA_READ_POLL_MS);

            if (btif_media_cb.peer_sep == AVDT_TSEP_SNK) {
                /* Start the media task to encode SBC */
                btif_media_task_start_aa_req();

                /* make sure we update any changed sbc encoder params */
                btif_a2dp_encoder_update();
            }
            btif_media_cb.data_channel_open = TRUE;

            /* ack back when media task is fully started */
            break;

        case UIPC_CLOSE_EVT:
            a2dp_cmd_acknowledge(A2DP_CTRL_ACK_SUCCESS);
            btif_audiopath_detached();
            btif_media_cb.data_channel_open = FALSE;
            break;

        default :
            APPL_TRACE_ERROR("### A2DP-DATA EVENT %d NOT HANDLED ###", event);
            break;
    }
}


/*****************************************************************************
 **  BTIF ADAPTATION
 *****************************************************************************/

static UINT16 btif_media_task_get_sbc_rate(void)
{
    UINT16 rate = DEFAULT_SBC_BITRATE;

    /* restrict bitrate if a2dp link is non-edr */
    if (!btif_av_is_peer_edr())
    {
        rate = BTIF_A2DP_NON_EDR_MAX_RATE;
        APPL_TRACE_DEBUG("non-edr a2dp sink detected, restrict rate to %d", rate);
    }

    return rate;
}

static void btif_a2dp_encoder_init(void)
{
    UINT16 minmtu;
    tBTIF_MEDIA_INIT_AUDIO msg;
    tA2D_SBC_CIE sbc_config;

    /* lookup table for converting channel mode */
    UINT16 codec_mode_tbl[5] = { SBC_JOINT_STEREO, SBC_STEREO, SBC_DUAL, 0, SBC_MONO };

    /* lookup table for converting number of blocks */
    UINT16 codec_block_tbl[5] = { 16, 12, 8, 0, 4 };

    /* lookup table to convert freq */
    UINT16 freq_block_tbl[5] = { SBC_sf48000, SBC_sf44100, SBC_sf32000, 0, SBC_sf16000 };

    APPL_TRACE_DEBUG("btif_a2dp_encoder_init");

    /* Retrieve the current SBC configuration (default if currently not used) */
    bta_av_co_audio_get_sbc_config(&sbc_config, &minmtu);
    msg.NumOfSubBands = (sbc_config.num_subbands == A2D_SBC_IE_SUBBAND_4) ? 4 : 8;
    msg.NumOfBlocks = codec_block_tbl[sbc_config.block_len >> 5];
    msg.AllocationMethod = (sbc_config.alloc_mthd == A2D_SBC_IE_ALLOC_MD_L) ? SBC_LOUDNESS : SBC_SNR;
    msg.ChannelMode = codec_mode_tbl[sbc_config.ch_mode >> 1];
    msg.SamplingFreq = freq_block_tbl[sbc_config.samp_freq >> 5];
    msg.MtuSize = minmtu;

    APPL_TRACE_EVENT("msg.ChannelMode %x", msg.ChannelMode);

    /* Init the media task to encode SBC properly */
    btif_media_task_enc_init_req(&msg);
}

static void btif_a2dp_encoder_update(void)
{
    UINT16 minmtu;
    tA2D_SBC_CIE sbc_config;
    tBTIF_MEDIA_UPDATE_AUDIO msg;
    UINT8 pref_min;
    UINT8 pref_max;

    APPL_TRACE_DEBUG("btif_a2dp_encoder_update");

    /* Retrieve the current SBC configuration (default if currently not used) */
    bta_av_co_audio_get_sbc_config(&sbc_config, &minmtu);

    APPL_TRACE_DEBUG("btif_a2dp_encoder_update: Common min_bitpool:%d(0x%x) max_bitpool:%d(0x%x)",
            sbc_config.min_bitpool, sbc_config.min_bitpool,
            sbc_config.max_bitpool, sbc_config.max_bitpool);

    if (sbc_config.min_bitpool > sbc_config.max_bitpool)
    {
        APPL_TRACE_ERROR("btif_a2dp_encoder_update: ERROR btif_a2dp_encoder_update min_bitpool > max_bitpool");
    }

    /* check if remote sink has a preferred bitpool range */
    if (bta_av_co_get_remote_bitpool_pref(&pref_min, &pref_max) == TRUE)
    {
        /* adjust our preferred bitpool with the remote preference if within
           our capable range */

        if (pref_min < sbc_config.min_bitpool)
            pref_min = sbc_config.min_bitpool;

        if (pref_max > sbc_config.max_bitpool)
            pref_max = sbc_config.max_bitpool;

        msg.MinBitPool = pref_min;
        msg.MaxBitPool = pref_max;

        if ((pref_min != sbc_config.min_bitpool) || (pref_max != sbc_config.max_bitpool))
        {
            APPL_TRACE_EVENT("## adjusted our bitpool range to peer pref [%d:%d] ##",
                pref_min, pref_max);
        }
    }
    else
    {
        msg.MinBitPool = sbc_config.min_bitpool;
        msg.MaxBitPool = sbc_config.max_bitpool;
    }

    msg.MinMtuSize = minmtu;

#ifdef BTA_AV_SPLIT_A2DP_ENABLED
        btif_media_cb.max_bitpool = msg.MaxBitPool;
        btif_media_cb.min_bitpool = msg.MinBitPool;
        APPL_TRACE_DEBUG("Updated min_bitpool: 0x%x max_bitpool: 0x%x",
            btif_media_cb.min_bitpool, btif_media_cb.max_bitpool);
#endif

    /* Update the media task to encode SBC properly */
    btif_media_task_enc_update_req(&msg);
}

bool btif_a2dp_start_media_task(void)
{
    if (media_task_running != MEDIA_TASK_STATE_OFF)
    {
        APPL_TRACE_ERROR("warning : media task already running");
        return false;
    }

    APPL_TRACE_IMP("## A2DP START MEDIA THREAD ##");

    btif_media_cmd_msg_queue = fixed_queue_new(SIZE_MAX);

    /* start a2dp media task */
    worker_thread = thread_new("media_worker");
    if (worker_thread == NULL)
        goto error_exit;

    fixed_queue_register_dequeue(btif_media_cmd_msg_queue,
        thread_get_reactor(worker_thread),
        btif_media_thread_handle_cmd,
        NULL);

    thread_post(worker_thread, btif_media_thread_init, NULL);

    APPL_TRACE_IMP("## A2DP MEDIA THREAD STARTED ##");

    return true;

 error_exit:;
    APPL_TRACE_ERROR("%s unable to start up media thread", __func__);
    return false;
}

void btif_a2dp_stop_media_task(void)
{
    APPL_TRACE_IMP("## A2DP STOP MEDIA THREAD ##");

    // Stop timer
    alarm_free(btif_media_cb.media_alarm);
    btif_media_cb.media_alarm = NULL;
    btif_media_cb.is_tx_timer = FALSE;

    // Exit thread
    fixed_queue_free(btif_media_cmd_msg_queue, NULL);
    thread_post(worker_thread, btif_media_thread_cleanup, NULL);
    thread_free(worker_thread);

    worker_thread = NULL;
    btif_media_cmd_msg_queue = NULL;
}

/*****************************************************************************
**
** Function        btif_a2dp_on_init
**
** Description
**
** Returns
**
*******************************************************************************/

void btif_a2dp_on_init(void)
{
    //tput_mon(1, 0, 1);
}


/*****************************************************************************
**
** Function        btif_a2dp_setup_codec
**
** Description     does codec setup
**
** Returns        tBTIF_STATUS
**
*******************************************************************************/

tBTIF_STATUS btif_a2dp_setup_codec(void)
{
    tBTIF_AV_MEDIA_FEEDINGS media_feeding;
    tBTIF_STATUS status;

    APPL_TRACE_EVENT("## A2DP SETUP CODEC ##");

    GKI_disable();

#if defined(SAMPLE_RATE_48K) || defined (BTA_AV_SPLIT_A2DP_DEF_FREQ_48KHZ)
    /* for now hardcode 44/48 khz 16 bit stereo */
    media_feeding.cfg.pcm.sampling_freq = 48000;
#else
    /* for now hardcode 44.1 khz 16 bit stereo PCM format */
    media_feeding.cfg.pcm.sampling_freq = 44100;
#endif
    media_feeding.cfg.pcm.bit_per_sample = 16;
    media_feeding.cfg.pcm.num_channel = 2;
    media_feeding.format = BTIF_AV_CODEC_PCM;

    if (bta_av_co_audio_set_codec(&media_feeding, &status))
    {
        tBTIF_MEDIA_INIT_AUDIO_FEEDING mfeed;

        /* Init the encoding task */
        btif_a2dp_encoder_init();

        /* Build the media task configuration */
        mfeed.feeding = media_feeding;
        mfeed.feeding_mode = BTIF_AV_FEEDING_ASYNCHRONOUS;
        /* Send message to Media task to configure transcoding */
        btif_media_task_audio_feeding_init_req(&mfeed);
    }
    else
    {
        status = BTIF_ERROR_SRV_AV_FEEDING_NOT_SUPPORTED;
    }
    GKI_enable();
    return status;
}


/*****************************************************************************
**
** Function        btif_a2dp_on_idle
**
** Description
**
** Returns
**
*******************************************************************************/

void btif_a2dp_on_idle(void)
{
    APPL_TRACE_IMP("## ON A2DP IDLE ##");
    if (btif_media_cb.peer_sep == AVDT_TSEP_SNK)
    {
        /* Make sure media task is stopped */
        btif_media_task_stop_aa_req();
    }
#ifdef BTA_AV_SPLIT_A2DP_ENABLED
    btif_media_send_reset_vendor_state();
#endif

    bta_av_co_init();
#if (BTA_AV_SINK_INCLUDED == TRUE)
    if (btif_media_cb.peer_sep == AVDT_TSEP_SRC)
    {
        btif_media_cb.rx_flush = TRUE;
        btif_media_task_aa_rx_flush_req();
        btif_media_task_aa_handle_stop_decoding();
        btif_media_task_clear_track();
#ifdef USE_AUDIO_TRACK
        btif_media_cb.rx_audio_focus_state = BTIF_MEDIA_FOCUS_IDLE;
#endif
        APPL_TRACE_DEBUG("Stopped BT track");
    }
#endif
}

/*****************************************************************************
**
** Function        btif_a2dp_on_open
**
** Description
**
** Returns
**
*******************************************************************************/

void btif_a2dp_on_open(void)
{
    APPL_TRACE_IMP("## ON A2DP OPEN ##");

#ifndef BTA_AV_SPLIT_A2DP_ENABLED
    /* always use callback to notify socket events */
    UIPC_Open(UIPC_CH_ID_AV_AUDIO, btif_a2dp_data_cb);
#endif
}

/*******************************************************************************
 **
 ** Function         btif_media_task_clear_track
 **
 ** Description
 **
 ** Returns          TRUE is success
 **
 *******************************************************************************/
BOOLEAN btif_media_task_clear_track(void)
{
    BT_HDR *p_buf;

    if (NULL == (p_buf = GKI_getbuf(sizeof(BT_HDR))))
    {
        return FALSE;
    }

    p_buf->event = BTIF_MEDIA_AUDIO_SINK_CLEAR_TRACK;

    if (btif_media_cmd_msg_queue != NULL)
        fixed_queue_enqueue(btif_media_cmd_msg_queue, p_buf);
    return TRUE;
}

/*****************************************************************************
**
** Function        btif_reset_decoder
**
** Description
**
** Returns
**
*******************************************************************************/

void btif_reset_decoder(UINT8 *p_av)
{
    APPL_TRACE_EVENT("btif_reset_decoder");
    APPL_TRACE_DEBUG("btif_reset_decoder p_codec_info[%x:%x:%x:%x:%x:%x]",
            p_av[1], p_av[2], p_av[3],
            p_av[4], p_av[5], p_av[6]);

    tBTIF_MEDIA_SINK_CFG_UPDATE *p_buf;
    if (NULL == (p_buf = GKI_getbuf(sizeof(tBTIF_MEDIA_SINK_CFG_UPDATE))))
    {
        APPL_TRACE_EVENT("btif_reset_decoder No Buffer ");
        return;
    }

    memcpy(p_buf->codec_info,p_av, AVDT_CODEC_SIZE);
    p_buf->hdr.event = BTIF_MEDIA_AUDIO_SINK_CFG_UPDATE;

    if (btif_media_cmd_msg_queue != NULL)
        fixed_queue_enqueue(btif_media_cmd_msg_queue, p_buf);
}

/*****************************************************************************
**
** Function        btif_a2dp_on_started
**
** Description
**
** Returns
**
*******************************************************************************/

BOOLEAN btif_a2dp_on_started(tBTA_AV_START *p_av, BOOLEAN pending_start)
{
    BOOLEAN ack = FALSE;

    APPL_TRACE_IMP("## ON A2DP STARTED ##");

    if (p_av == NULL)
    {
#ifdef BTA_AV_SPLIT_A2DP_ENABLED
        if (btif_media_cb.peer_sep == AVDT_TSEP_SNK)
        {
            btif_media_on_start_vendor_command();
        }
#else
        /* ack back a local start request */
        a2dp_cmd_acknowledge(A2DP_CTRL_ACK_SUCCESS);
#endif
        return TRUE;
    }

    if (p_av->status == BTA_AV_SUCCESS)
    {
        if (p_av->suspending == FALSE)
        {
            if (p_av->initiator)
            {
                if (pending_start) {
#ifdef BTA_AV_SPLIT_A2DP_ENABLED
                    if (btif_media_cb.peer_sep == AVDT_TSEP_SNK)
                    {
                        btif_media_on_start_vendor_command();
                    }
#else
                    a2dp_cmd_acknowledge(A2DP_CTRL_ACK_SUCCESS);
#endif
                    ack = TRUE;
                }
            }
            else
            {
                /* we were remotely started,  make sure codec
                   is setup before datapath is started */
#ifdef BTA_AV_SPLIT_A2DP_ENABLED
                if (btif_media_cb.peer_sep == AVDT_TSEP_SNK)
                {
                    APPL_TRACE_IMP("start VS command exchange on remote start");
                    btif_media_on_start_vendor_command();
                }
#else
                btif_a2dp_setup_codec();
#endif
            }

            /* media task is autostarted upon a2dp audiopath connection */
        }
    }
    else if (pending_start)
    {
        a2dp_cmd_acknowledge(A2DP_CTRL_ACK_FAILURE);
        ack = TRUE;
    }
    return ack;
}


/*****************************************************************************
**
** Function        btif_a2dp_ack_fail
**
** Description
**
** Returns
**
*******************************************************************************/

void btif_a2dp_ack_fail(void)
{
    APPL_TRACE_IMP("## A2DP_CTRL_ACK_FAILURE ##");
    a2dp_cmd_acknowledge(A2DP_CTRL_ACK_FAILURE);
}

/*****************************************************************************
**
** Function        btif_a2dp_on_stopped
**
** Description
**
** Returns
**
*******************************************************************************/

void btif_a2dp_on_stopped(tBTA_AV_SUSPEND *p_av)
{
    APPL_TRACE_IMP("## ON A2DP STOPPED ##");
    if (btif_media_cb.peer_sep == AVDT_TSEP_SRC) /*  Handling for A2DP SINK cases*/
    {
        btif_media_cb.rx_flush = TRUE;
        btif_media_task_aa_rx_flush_req();
        btif_media_task_aa_handle_stop_decoding();
#ifndef USE_AUDIO_TRACK
        UIPC_Close(UIPC_CH_ID_AV_AUDIO);
#endif
        btif_media_cb.data_channel_open = FALSE;
        return;
    }
    /* allow using this api for other than suspend */
    if (p_av != NULL)
    {
        if (p_av->status != BTA_AV_SUCCESS)
        {
            APPL_TRACE_EVENT("AV STOP FAILED (%d)", p_av->status);

            if (p_av->initiator)
                a2dp_cmd_acknowledge(A2DP_CTRL_ACK_FAILURE);
            return;
        }
    }

    /* ensure tx frames are immediately suspended */
    btif_media_cb.tx_flush = 1;

    /* request to stop media task  */
#ifndef BTA_AV_SPLIT_A2DP_ENABLED
    btif_media_task_aa_tx_flush_req();
#endif
    btif_media_task_stop_aa_req();

    /* once stream is fully stopped we will ack back */
}


/*****************************************************************************
**
** Function        btif_a2dp_on_suspended
**
** Description
**
** Returns
**
*******************************************************************************/

void btif_a2dp_on_suspended(tBTA_AV_SUSPEND *p_av)
{
    APPL_TRACE_IMP("## ON A2DP SUSPENDED ##");
    if (btif_media_cb.peer_sep == AVDT_TSEP_SRC)
    {
        btif_media_cb.rx_flush = TRUE;
        btif_media_task_aa_rx_flush_req();
        btif_media_task_aa_handle_stop_decoding();
#ifndef USE_AUDIO_TRACK
        UIPC_Close(UIPC_CH_ID_AV_AUDIO);
#endif
        return;
    }

    /* check for status failures */
    if (p_av->status != BTA_AV_SUCCESS)
    {
        if (p_av->initiator == TRUE)
            a2dp_cmd_acknowledge(A2DP_CTRL_ACK_FAILURE);
    }

    /* once stream is fully stopped we will ack back */

    /* ensure tx frames are immediately flushed */
    btif_media_cb.tx_flush = 1;

    /* stop timer tick */
    btif_media_task_stop_aa_req();
}

/* when true media task discards any rx frames */
void btif_a2dp_set_rx_flush(BOOLEAN enable)
{
    APPL_TRACE_EVENT("## DROP RX %d ##", enable);
    btif_media_cb.rx_flush = enable;
}

/* when true media task discards any tx frames */
void btif_a2dp_set_tx_flush(BOOLEAN enable)
{
    APPL_TRACE_EVENT("## DROP TX %d ##", enable);
    btif_media_cb.tx_flush = enable;
}
#ifdef USE_AUDIO_TRACK
void btif_a2dp_set_audio_focus_state(btif_media_audio_focus_state state)
{
    APPL_TRACE_EVENT("btif_a2dp_set_audio_focus_state");
    tBTIF_MEDIA_SINK_FOCUS_UPDATE *p_buf;
    if (NULL == (p_buf = GKI_getbuf(sizeof(tBTIF_MEDIA_SINK_FOCUS_UPDATE))))
    {
        APPL_TRACE_EVENT("btif_a2dp_set_audio_focus_state No Buffer ");
        return;
    }

    p_buf->focus_state = state;
    p_buf->hdr.event = BTIF_MEDIA_AUDIO_SINK_SET_FOCUS_STATE;
    if (btif_media_cmd_msg_queue != NULL)
        fixed_queue_enqueue(btif_media_cmd_msg_queue, p_buf);
}
#endif
#if (BTA_AV_SINK_INCLUDED == TRUE)
static void btif_media_task_avk_handle_timer(UNUSED_ATTR void *context)
{
    UINT8 count;
    tBT_SBC_HDR *p_msg;
    int num_sbc_frames;
    int num_frames_to_process;

    count = btif_media_cb.RxSbcQ._count;
    if (0 == count)
    {
        APPL_TRACE_DEBUG("  QUE  EMPTY ");
    }
    else
    {
#ifdef USE_AUDIO_TRACK
        switch(btif_media_cb.rx_audio_focus_state)
        {
            /* Don't Do anything in case of Idle, Requested */
            case BTIF_MEIDA_FOCUS_REQUESTED:
            case BTIF_MEDIA_FOCUS_IDLE:
                return;
            break;
            /* In case of Ready, request for focus and wait to move in granted */
            case BTIF_MEIDA_FOCUS_READY:
                btif_queue_focus_rquest();
                btif_media_cb.rx_audio_focus_state = BTIF_MEIDA_FOCUS_REQUESTED;
                return;
            break;
            /* play only in this case */
            case BTIF_MEIDA_FOCUS_GRANTED:
            break;
        }
#endif
        if (btif_media_cb.rx_flush == TRUE)
        {
            btif_media_flush_q(&(btif_media_cb.RxSbcQ));
            return;
        }

        num_frames_to_process = btif_media_cb.frames_to_process;
        APPL_TRACE_DEBUG(" Process Frames + ");

        do
        {
            p_msg = (tBT_SBC_HDR *)GKI_getfirst(&(btif_media_cb.RxSbcQ));
            if (p_msg == NULL)
                return;
            num_sbc_frames  = p_msg->num_frames_to_be_processed; /* num of frames in Que Packets */
            APPL_TRACE_DEBUG(" Frames left in topmost packet %d", num_sbc_frames);
            APPL_TRACE_DEBUG(" Remaining frames to process in tick %d", num_frames_to_process);
            APPL_TRACE_DEBUG(" Num of Packets in Que %d", btif_media_cb.RxSbcQ._count);

            if ( num_sbc_frames > num_frames_to_process) /*  Que Packet has more frames*/
            {
                 p_msg->num_frames_to_be_processed= num_frames_to_process;
                 btif_media_task_handle_inc_media(p_msg);
                 p_msg->num_frames_to_be_processed = num_sbc_frames - num_frames_to_process;
                 num_frames_to_process = 0;
                 break;
            }
            else                                        /*  Que packet has less frames */
            {
                btif_media_task_handle_inc_media(p_msg);
                p_msg = (tBT_SBC_HDR *)GKI_dequeue(&(btif_media_cb.RxSbcQ));
                if( p_msg == NULL )
                {
                     APPL_TRACE_ERROR("Insufficient data in que ");
                     break;
                }
                num_frames_to_process = num_frames_to_process - p_msg->num_frames_to_be_processed;
                GKI_freebuf(p_msg);
            }
        }while(num_frames_to_process > 0);

        APPL_TRACE_DEBUG(" Process Frames - ");
    }
}
#else
static void btif_media_task_avk_handle_timer(UNUSED_ATTR void *context) {}
#endif

static void btif_media_task_aa_handle_timer(UNUSED_ATTR void *context)
{
    log_tstamps_us("media task tx timer");

#if (BTA_AV_INCLUDED == TRUE)
    if(btif_media_cb.is_tx_timer == TRUE)
    {
        btif_media_send_aa_frame();
    }
    else
    {
        APPL_TRACE_ERROR("ERROR Media task Scheduled after Suspend");
    }
#endif
}

#if (BTA_AV_INCLUDED == TRUE)
static void btif_media_task_aa_handle_uipc_rx_rdy(void)
{
    /* process all the UIPC data */
    btif_media_aa_prep_2_send(0xFF);

    /* send it */
    LOG_VERBOSE("btif_media_task_aa_handle_uipc_rx_rdy calls bta_av_ci_src_data_ready");
    bta_av_ci_src_data_ready(BTA_AV_CHNL_AUDIO);
}
#endif

static void btif_media_thread_init(UNUSED_ATTR void *context) {
  APPL_TRACE_IMP(" btif_media_thread_init");
  memset(&btif_media_cb, 0, sizeof(btif_media_cb));
  UIPC_Init(NULL);

#if (BTA_AV_INCLUDED == TRUE)
  UIPC_Open(UIPC_CH_ID_AV_CTRL , btif_a2dp_ctrl_cb);
#endif

  raise_priority_a2dp(TASK_HIGH_MEDIA);
  media_task_running = MEDIA_TASK_STATE_ON;
}

static void btif_media_thread_cleanup(UNUSED_ATTR void *context) {
  APPL_TRACE_IMP(" btif_media_thread_cleanup");
  /* make sure no channels are restarted while shutting down */
  media_task_running = MEDIA_TASK_STATE_SHUTTING_DOWN;

  /* this calls blocks until uipc is fully closed */
  UIPC_Close(UIPC_CH_ID_ALL);

  /* Clear media task flag */
  media_task_running = MEDIA_TASK_STATE_OFF;
}

/*******************************************************************************
 **
 ** Function         btif_media_task_send_cmd_evt
 **
 ** Description
 **
 ** Returns          TRUE is success
 **
 *******************************************************************************/
BOOLEAN btif_media_task_send_cmd_evt(UINT16 Evt)
{
    BT_HDR *p_buf;
    if (NULL == (p_buf = GKI_getbuf(sizeof(BT_HDR))))
    {
        return FALSE;
    }

    p_buf->event = Evt;

    if (btif_media_cmd_msg_queue != NULL)
        fixed_queue_enqueue(btif_media_cmd_msg_queue, p_buf);
    return TRUE;
}

/*******************************************************************************
 **
 ** Function         btif_media_flush_q
 **
 ** Description
 **
 ** Returns          void
 **
 *******************************************************************************/
static void btif_media_flush_q(BUFFER_Q *p_q)
{
    while (!GKI_queue_is_empty(p_q))
    {
        GKI_freebuf(GKI_dequeue(p_q));
    }
}

static void btif_media_thread_handle_cmd(fixed_queue_t *queue, UNUSED_ATTR void *context)
{
    BT_HDR *p_msg = (BT_HDR *)fixed_queue_dequeue(queue);
    APPL_TRACE_IMP("btif_media_thread_handle_cmd : %d %s", p_msg->event,
             dump_media_event(p_msg->event));

    switch (p_msg->event)
    {
#if (BTA_AV_INCLUDED == TRUE)
    case BTIF_MEDIA_START_AA_TX:
        btif_media_task_aa_start_tx();
        break;
    case BTIF_MEDIA_STOP_AA_TX:
        btif_media_task_aa_stop_tx();
        break;
    case BTIF_MEDIA_SBC_ENC_INIT:
        btif_media_task_enc_init(p_msg);
        break;
    case BTIF_MEDIA_SBC_ENC_UPDATE:
        btif_media_task_enc_update(p_msg);
        break;
    case BTIF_MEDIA_AUDIO_FEEDING_INIT:
        btif_media_task_audio_feeding_init(p_msg);
        break;
    case BTIF_MEDIA_FLUSH_AA_TX:
        btif_media_task_aa_tx_flush(p_msg);
        break;
    case BTIF_MEDIA_UIPC_RX_RDY:
        btif_media_task_aa_handle_uipc_rx_rdy();
        break;
#ifdef USE_AUDIO_TRACK
    case BTIF_MEDIA_AUDIO_SINK_SET_FOCUS_STATE:
        if(!btif_av_is_connected())
            break;
        btif_media_cb.rx_audio_focus_state = ((tBTIF_MEDIA_SINK_FOCUS_UPDATE *)p_msg)->focus_state;
        APPL_TRACE_DEBUG("Setting focus state to %d ",btif_media_cb.rx_audio_focus_state);
        break;
#endif
    case BTIF_MEDIA_AUDIO_SINK_CFG_UPDATE:
#if (BTA_AV_SINK_INCLUDED == TRUE)
        btif_media_task_aa_handle_decoder_reset(p_msg);
#endif
        break;
    case BTIF_MEDIA_AUDIO_SINK_CLEAR_TRACK:
#if (BTA_AV_SINK_INCLUDED == TRUE)
        btif_media_task_aa_handle_clear_track();
#endif
        break;
     case BTIF_MEDIA_FLUSH_AA_RX:
        btif_media_task_aa_rx_flush();
        break;
#ifdef BTA_AV_SPLIT_A2DP_ENABLED
    case BTIF_MEDIA_RESET_VS_STATE:
        btif_media_cb.tx_started = FALSE;
        btif_media_cb.tx_stop_initiated = FALSE;
        btif_media_cb.vs_configs_exchanged = FALSE;
        break;
    case BTIF_MEDIA_START_VS_CMD:
        btif_a2dp_encoder_update();
        btif_media_start_vendor_command();
        break;
    case BTIF_MEDIA_STOP_VS_CMD:
        btif_media_send_vendor_stop();
        break;
    case BTIF_MEDIA_VS_A2DP_START_SUCCESS:
        btif_media_cb.tx_started = TRUE;
        a2dp_cmd_acknowledge(A2DP_CTRL_ACK_SUCCESS);
        break;
    case BTIF_MEDIA_VS_A2DP_STOP_SUCCESS:
        btif_media_cb.tx_started = FALSE;
        btif_media_cb.tx_stop_initiated = FALSE;
        a2dp_cmd_acknowledge(A2DP_CTRL_ACK_SUCCESS);
        break;
    case BTIF_MEDIA_VS_A2DP_STOP_FAILURE:
        btif_media_cb.tx_stop_initiated = FALSE;
        a2dp_cmd_acknowledge(A2DP_CTRL_ACK_FAILURE);
        break;
    case BTIF_MEDIA_VS_A2DP_MEDIA_CHNL_CFG_SUCCESS:
        btif_media_send_vendor_pref_bit_rate();
        break;
    case BTIF_MEDIA_VS_A2DP_WRITE_SBC_CFG_SUCCESS:
        btif_media_send_vendor_media_chn_cfg();
        break;
    case BTIF_MEDIA_VS_A2DP_PREF_BIT_RATE_SUCCESS:
#if (BTA_AV_CO_CP_SCMS_T == TRUE)
        btif_media_send_vendor_scmst_hdr();
#else
        if (!btif_media_cb.vs_configs_exchanged)
            btif_media_cb.vs_configs_exchanged = TRUE;
        btif_media_send_vendor_start();
#endif
        break;
#if (BTA_AV_CO_CP_SCMS_T == TRUE)
    case BTIF_MEDIA_VS_A2DP_SET_SCMST_HDR_SUCCESS:
        if (!btif_media_cb.vs_configs_exchanged)
            btif_media_cb.vs_configs_exchanged = TRUE;
        btif_media_send_vendor_start();
        break;
#endif
#endif
#endif
    default:
        APPL_TRACE_ERROR("ERROR in %s unknown event %d", __func__, p_msg->event);
    }
    GKI_freebuf(p_msg);
    APPL_TRACE_IMP("%s: %s DONE", __func__, dump_media_event(p_msg->event));
}

#if (BTA_AV_SINK_INCLUDED == TRUE)
/*******************************************************************************
 **
 ** Function         btif_media_task_handle_inc_media
 **
 ** Description
 **
 ** Returns          void
 **
 *******************************************************************************/
static void btif_media_task_handle_inc_media(tBT_SBC_HDR*p_msg)
{
    UINT8 *sbc_start_frame = ((UINT8*)(p_msg + 1) + p_msg->offset + 1);
    int count;
    UINT32 pcmBytes, availPcmBytes;
    OI_INT16 *pcmDataPointer = pcmData; /*Will be overwritten on next packet receipt*/
    OI_STATUS status;
    int num_sbc_frames = p_msg->num_frames_to_be_processed;
    UINT32 sbc_frame_len = p_msg->len - 1;
    availPcmBytes = sizeof(pcmData);

#ifdef USE_AUDIO_TRACK
    int retwriteAudioTrack = 0;
#endif
    if ((btif_media_cb.peer_sep == AVDT_TSEP_SNK) || (btif_media_cb.rx_flush))
    {
        APPL_TRACE_DEBUG(" State Changed happened in this tick ");
        return;
    }
#ifndef USE_AUDIO_TRACK
    // ignore data if no one is listening
    if (!btif_media_cb.data_channel_open)
    {
        APPL_TRACE_ERROR(" btif_media_task_handle_inc_media Channel not open, returning");
        return;
    }
#endif
    APPL_TRACE_DEBUG("Number of sbc frames %d, frame_len %d", num_sbc_frames, sbc_frame_len);

    for(count = 0; count < num_sbc_frames && sbc_frame_len != 0; count ++)
    {
        pcmBytes = availPcmBytes;
        status = OI_CODEC_SBC_DecodeFrame(&context, (const OI_BYTE**)&sbc_start_frame,
                                                        (OI_UINT32 *)&sbc_frame_len,
                                                        (OI_INT16 *)pcmDataPointer,
                                                        (OI_UINT32 *)&pcmBytes);
        if (!OI_SUCCESS(status)) {
            APPL_TRACE_ERROR("Decoding failure: %d\n", status);
            break;
        }
        availPcmBytes -= pcmBytes;
        pcmDataPointer += pcmBytes/2;
        p_msg->offset += (p_msg->len - 1) - sbc_frame_len;
        p_msg->len = sbc_frame_len + 1;
    }

#ifdef USE_AUDIO_TRACK
    retwriteAudioTrack = btWriteData((void*)pcmData, (sizeof(pcmData) - availPcmBytes));
#else
    UIPC_Send(UIPC_CH_ID_AV_AUDIO, 0, (UINT8 *)pcmData, (sizeof(pcmData) - availPcmBytes));
#endif
    APPL_TRACE_LATENCY_AUDIO("Written to audio, seq number %d", p_msg->layer_specific);
}
#endif

#if (BTA_AV_INCLUDED == TRUE)
/*******************************************************************************
 **
 ** Function         btif_media_task_enc_init_req
 **
 ** Description
 **
 ** Returns          TRUE is success
 **
 *******************************************************************************/
BOOLEAN btif_media_task_enc_init_req(tBTIF_MEDIA_INIT_AUDIO *p_msg)
{
    tBTIF_MEDIA_INIT_AUDIO *p_buf;
    if (NULL == (p_buf = GKI_getbuf(sizeof(tBTIF_MEDIA_INIT_AUDIO))))
    {
        return FALSE;
    }

    memcpy(p_buf, p_msg, sizeof(tBTIF_MEDIA_INIT_AUDIO));
    p_buf->hdr.event = BTIF_MEDIA_SBC_ENC_INIT;

    if (btif_media_cmd_msg_queue != NULL)
        fixed_queue_enqueue(btif_media_cmd_msg_queue, p_buf);
    return TRUE;
}

/*******************************************************************************
 **
 ** Function         btif_media_task_enc_update_req
 **
 ** Description
 **
 ** Returns          TRUE is success
 **
 *******************************************************************************/
BOOLEAN btif_media_task_enc_update_req(tBTIF_MEDIA_UPDATE_AUDIO *p_msg)
{
    tBTIF_MEDIA_UPDATE_AUDIO *p_buf;
    if (NULL == (p_buf = GKI_getbuf(sizeof(tBTIF_MEDIA_UPDATE_AUDIO))))
    {
        return FALSE;
    }

    memcpy(p_buf, p_msg, sizeof(tBTIF_MEDIA_UPDATE_AUDIO));
    p_buf->hdr.event = BTIF_MEDIA_SBC_ENC_UPDATE;

    if (btif_media_cmd_msg_queue != NULL)
        fixed_queue_enqueue(btif_media_cmd_msg_queue, p_buf);
    return TRUE;
}

/*******************************************************************************
 **
 ** Function         btif_media_task_audio_feeding_init_req
 **
 ** Description
 **
 ** Returns          TRUE is success
 **
 *******************************************************************************/
BOOLEAN btif_media_task_audio_feeding_init_req(tBTIF_MEDIA_INIT_AUDIO_FEEDING *p_msg)
{
    tBTIF_MEDIA_INIT_AUDIO_FEEDING *p_buf;
    if (NULL == (p_buf = GKI_getbuf(sizeof(tBTIF_MEDIA_INIT_AUDIO_FEEDING))))
    {
        return FALSE;
    }

    memcpy(p_buf, p_msg, sizeof(tBTIF_MEDIA_INIT_AUDIO_FEEDING));
    p_buf->hdr.event = BTIF_MEDIA_AUDIO_FEEDING_INIT;

    if (btif_media_cmd_msg_queue != NULL)
        fixed_queue_enqueue(btif_media_cmd_msg_queue, p_buf);
    return TRUE;
}

/*******************************************************************************
 **
 ** Function         btif_media_task_start_aa_req
 **
 ** Description
 **
 ** Returns          TRUE is success
 **
 *******************************************************************************/
BOOLEAN btif_media_task_start_aa_req(void)
{
    BT_HDR *p_buf;
    if (NULL == (p_buf = GKI_getbuf(sizeof(BT_HDR))))
    {
        APPL_TRACE_EVENT("GKI failed");
        return FALSE;
    }

    p_buf->event = BTIF_MEDIA_START_AA_TX;

    if (btif_media_cmd_msg_queue != NULL)
        fixed_queue_enqueue(btif_media_cmd_msg_queue, p_buf);
    return TRUE;
}

/*******************************************************************************
 **
 ** Function         btif_media_task_stop_aa_req
 **
 ** Description
 **
 ** Returns          TRUE is success
 **
 *******************************************************************************/
BOOLEAN btif_media_task_stop_aa_req(void)
{
    BT_HDR *p_buf;
    if (NULL == (p_buf = GKI_getbuf(sizeof(BT_HDR))))
    {
        return FALSE;
    }

    p_buf->event = BTIF_MEDIA_STOP_AA_TX;

    /*
     * Explicitly check whether the btif_media_cmd_msg_queue is not NULL to
     * avoid a race condition during shutdown of the Bluetooth stack.
     * This race condition is triggered when A2DP audio is streaming on
     * shutdown:
     * "btif_a2dp_on_stopped() -> btif_media_task_stop_aa_req()" is called
     * to stop the particular audio stream, and this happens right after
     * the "cleanup() -> btif_a2dp_stop_media_task()" processing during
     * the shutdown of the Bluetooth stack.
     */
    if (btif_media_cmd_msg_queue != NULL)
        fixed_queue_enqueue(btif_media_cmd_msg_queue, p_buf);

    return TRUE;
}
/*******************************************************************************
 **
 ** Function         btif_media_task_aa_rx_flush_req
 **
 ** Description
 **
 ** Returns          TRUE is success
 **
 *******************************************************************************/
BOOLEAN btif_media_task_aa_rx_flush_req(void)
{
    BT_HDR *p_buf;

    if (GKI_queue_is_empty(&(btif_media_cb.RxSbcQ))== TRUE) /*  Que is already empty */
        return TRUE;

    if (NULL == (p_buf = GKI_getbuf(sizeof(BT_HDR))))
    {
        return FALSE;
    }

    p_buf->event = BTIF_MEDIA_FLUSH_AA_RX;

    if (btif_media_cmd_msg_queue != NULL)
        fixed_queue_enqueue(btif_media_cmd_msg_queue, p_buf);
    return TRUE;
}

/*******************************************************************************
 **
 ** Function         btif_media_task_aa_tx_flush_req
 **
 ** Description
 **
 ** Returns          TRUE is success
 **
 *******************************************************************************/
BOOLEAN btif_media_task_aa_tx_flush_req(void)
{
    BT_HDR *p_buf = GKI_getbuf(sizeof(BT_HDR));

    if (p_buf == NULL)
        return FALSE;

    p_buf->event = BTIF_MEDIA_FLUSH_AA_TX;

    /*
     * Explicitly check whether the btif_media_cmd_msg_queue is not NULL to
     * avoid a race condition during shutdown of the Bluetooth stack.
     * This race condition is triggered when A2DP audio is streaming on
     * shutdown:
     * "btif_a2dp_on_stopped() -> btif_media_task_aa_tx_flush_req()" is called
     * to stop the particular audio stream, and this happens right after
     * the "cleanup() -> btif_a2dp_stop_media_task()" processing during
     * the shutdown of the Bluetooth stack.
     */
    if (btif_media_cmd_msg_queue != NULL)
        fixed_queue_enqueue(btif_media_cmd_msg_queue, p_buf);

    return TRUE;
}
/*******************************************************************************
 **
 ** Function         btif_media_task_aa_rx_flush
 **
 ** Description
 **
 ** Returns          void
 **
 *******************************************************************************/
static void btif_media_task_aa_rx_flush(void)
{
    /* Flush all enqueued GKI SBC  buffers (encoded) */
    APPL_TRACE_DEBUG("btif_media_task_aa_rx_flush");

    btif_media_flush_q(&(btif_media_cb.RxSbcQ));
}


/*******************************************************************************
 **
 ** Function         btif_media_task_aa_tx_flush
 **
 ** Description
 **
 ** Returns          void
 **
 *******************************************************************************/
static void btif_media_task_aa_tx_flush(BT_HDR *p_msg)
{
    UNUSED(p_msg);

    /* Flush all enqueued GKI music buffers (encoded) */
    APPL_TRACE_DEBUG("btif_media_task_aa_tx_flush");

    btif_media_cb.media_feeding_state.pcm.counter = 0;
    btif_media_cb.media_feeding_state.pcm.aa_feed_residue = 0;

    btif_media_flush_q(&(btif_media_cb.TxAaQ));

    UIPC_Ioctl(UIPC_CH_ID_AV_AUDIO, UIPC_REQ_RX_FLUSH, NULL);
}

/*******************************************************************************
 **
 ** Function       btif_media_task_enc_init
 **
 ** Description    Initialize encoding task
 **
 ** Returns        void
 **
 *******************************************************************************/
static void btif_media_task_enc_init(BT_HDR *p_msg)
{
    tBTIF_MEDIA_INIT_AUDIO *pInitAudio = (tBTIF_MEDIA_INIT_AUDIO *) p_msg;

    APPL_TRACE_DEBUG("btif_media_task_enc_init");

    btif_media_cb.timestamp = 0;

    /* SBC encoder config (enforced even if not used) */
    btif_media_cb.encoder.s16ChannelMode = pInitAudio->ChannelMode;
    btif_media_cb.encoder.s16NumOfSubBands = pInitAudio->NumOfSubBands;
    btif_media_cb.encoder.s16NumOfBlocks = pInitAudio->NumOfBlocks;
    btif_media_cb.encoder.s16AllocationMethod = pInitAudio->AllocationMethod;
    btif_media_cb.encoder.s16SamplingFreq = pInitAudio->SamplingFreq;

    btif_media_cb.encoder.u16BitRate = btif_media_task_get_sbc_rate();

    /* Default transcoding is PCM to SBC, modified by feeding configuration */
    btif_media_cb.TxTranscoding = BTIF_MEDIA_TRSCD_PCM_2_SBC;
    btif_media_cb.TxAaMtuSize = ((BTIF_MEDIA_AA_BUF_SIZE-BTIF_MEDIA_AA_SBC_OFFSET-sizeof(BT_HDR))
            < pInitAudio->MtuSize) ? (BTIF_MEDIA_AA_BUF_SIZE - BTIF_MEDIA_AA_SBC_OFFSET
            - sizeof(BT_HDR)) : pInitAudio->MtuSize;

    APPL_TRACE_EVENT("btif_media_task_enc_init busy %d, mtu %d, peer mtu %d",
                     btif_media_cb.busy_level, btif_media_cb.TxAaMtuSize, pInitAudio->MtuSize);
    APPL_TRACE_EVENT("      ch mode %d, subnd %d, nb blk %d, alloc %d, rate %d, freq %d",
            btif_media_cb.encoder.s16ChannelMode, btif_media_cb.encoder.s16NumOfSubBands,
            btif_media_cb.encoder.s16NumOfBlocks,
            btif_media_cb.encoder.s16AllocationMethod, btif_media_cb.encoder.u16BitRate,
            btif_media_cb.encoder.s16SamplingFreq);

#ifndef BTA_AV_SPLIT_A2DP_ENABLED
    /* Reset entirely the SBC encoder */
    SBC_Encoder_Init(&(btif_media_cb.encoder));
#endif
    btif_media_cb.TxNumSBCFrames = check_for_max_number_of_frames_per_packet();
    APPL_TRACE_DEBUG("btif_media_task_enc_init bit pool %d", btif_media_cb.encoder.s16BitPool);
}

/*******************************************************************************
 **
 ** Function       btif_media_task_enc_update
 **
 ** Description    Update encoding task
 **
 ** Returns        void
 **
 *******************************************************************************/

static void btif_media_task_enc_update(BT_HDR *p_msg)
{
    tBTIF_MEDIA_UPDATE_AUDIO * pUpdateAudio = (tBTIF_MEDIA_UPDATE_AUDIO *) p_msg;
    SBC_ENC_PARAMS *pstrEncParams = &btif_media_cb.encoder;
    UINT16 s16SamplingFreq;
    SINT16 s16BitPool = 0;
    SINT16 s16BitRate;
    SINT16 s16FrameLen;
    UINT8 protect = 0;

    APPL_TRACE_DEBUG("btif_media_task_enc_update : minmtu %d, maxbp %d minbp %d",
            pUpdateAudio->MinMtuSize, pUpdateAudio->MaxBitPool, pUpdateAudio->MinBitPool);

    if (!pstrEncParams->s16NumOfSubBands)
    {
        APPL_TRACE_ERROR("Error: SubBands are set to 0, resetting to Max");
        pstrEncParams->s16NumOfSubBands = SBC_MAX_NUM_OF_SUBBANDS;
    }
    if (!pstrEncParams->s16NumOfBlocks)
    {
        APPL_TRACE_ERROR("Error: Blocks are set to 0, resetting to Max");
        pstrEncParams->s16NumOfBlocks = SBC_MAX_NUM_OF_BLOCKS;
    }
    if (!pstrEncParams->s16NumOfChannels)
    {
        APPL_TRACE_ERROR("Error: Channels are set to 0, resetting to Max");
        pstrEncParams->s16NumOfChannels = SBC_MAX_NUM_OF_CHANNELS;
    }
    /* Only update the bitrate and MTU size while timer is running to make sure it has been initialized */
    //if (btif_media_cb.is_tx_timer)
    {
        btif_media_cb.TxAaMtuSize = ((BTIF_MEDIA_AA_BUF_SIZE -
                                      BTIF_MEDIA_AA_SBC_OFFSET - sizeof(BT_HDR))
                < pUpdateAudio->MinMtuSize) ? (BTIF_MEDIA_AA_BUF_SIZE - BTIF_MEDIA_AA_SBC_OFFSET
                - sizeof(BT_HDR)) : pUpdateAudio->MinMtuSize;
        /* Set the initial target bit rate */
        pstrEncParams->u16BitRate = btif_media_task_get_sbc_rate();

        if (pstrEncParams->s16SamplingFreq == SBC_sf16000)
            s16SamplingFreq = 16000;
        else if (pstrEncParams->s16SamplingFreq == SBC_sf32000)
            s16SamplingFreq = 32000;
        else if (pstrEncParams->s16SamplingFreq == SBC_sf44100)
            s16SamplingFreq = 44100;
        else
            s16SamplingFreq = 48000;

        do
        {
            if (pstrEncParams->s16NumOfBlocks == 0 || pstrEncParams->s16NumOfSubBands == 0
                || pstrEncParams->s16NumOfChannels == 0)
            {
                APPL_TRACE_ERROR("btif_media_task_enc_update() - Avoiding division by zero...");
                APPL_TRACE_ERROR("btif_media_task_enc_update() - block=%d, subBands=%d, channels=%d",
                    pstrEncParams->s16NumOfBlocks, pstrEncParams->s16NumOfSubBands,
                    pstrEncParams->s16NumOfChannels);
                break;
            }

            if ((pstrEncParams->s16ChannelMode == SBC_JOINT_STEREO) ||
                (pstrEncParams->s16ChannelMode == SBC_STEREO) )
            {
                s16BitPool = (SINT16)( (pstrEncParams->u16BitRate *
                    pstrEncParams->s16NumOfSubBands * 1000 / s16SamplingFreq)
                    -( (32 + (4 * pstrEncParams->s16NumOfSubBands *
                    pstrEncParams->s16NumOfChannels)
                    + ( (pstrEncParams->s16ChannelMode - 2) *
                    pstrEncParams->s16NumOfSubBands )   )
                    / pstrEncParams->s16NumOfBlocks) );

                s16FrameLen = 4 + (4*pstrEncParams->s16NumOfSubBands*
                    pstrEncParams->s16NumOfChannels)/8
                    + ( ((pstrEncParams->s16ChannelMode - 2) *
                    pstrEncParams->s16NumOfSubBands)
                    + (pstrEncParams->s16NumOfBlocks * s16BitPool) ) / 8;

                s16BitRate = (8 * s16FrameLen * s16SamplingFreq)
                    / (pstrEncParams->s16NumOfSubBands *
                    pstrEncParams->s16NumOfBlocks * 1000);

                if (s16BitRate > pstrEncParams->u16BitRate)
                    s16BitPool--;

                if(pstrEncParams->s16NumOfSubBands == 8)
                    s16BitPool = (s16BitPool > 255) ? 255 : s16BitPool;
                else
                    s16BitPool = (s16BitPool > 128) ? 128 : s16BitPool;
            }
            else
            {
                s16BitPool = (SINT16)( ((pstrEncParams->s16NumOfSubBands *
                    pstrEncParams->u16BitRate * 1000)
                    / (s16SamplingFreq * pstrEncParams->s16NumOfChannels))
                    -( ( (32 / pstrEncParams->s16NumOfChannels) +
                    (4 * pstrEncParams->s16NumOfSubBands) )
                    /   pstrEncParams->s16NumOfBlocks ) );

                pstrEncParams->s16BitPool = (s16BitPool >
                    (16 * pstrEncParams->s16NumOfSubBands))
                    ? (16*pstrEncParams->s16NumOfSubBands) : s16BitPool;
            }

            if (s16BitPool < 0)
            {
                s16BitPool = 0;
            }

            APPL_TRACE_EVENT("bitpool candidate : %d (%d kbps)",
                         s16BitPool, pstrEncParams->u16BitRate);

            if (s16BitPool > pUpdateAudio->MaxBitPool)
            {
                APPL_TRACE_DEBUG("btif_media_task_enc_update computed bitpool too large (%d)",
                                    s16BitPool);
                /* Decrease bitrate */
                btif_media_cb.encoder.u16BitRate -= BTIF_MEDIA_BITRATE_STEP;
                /* Record that we have decreased the bitrate */
                protect |= 1;
            }
            else if (s16BitPool < pUpdateAudio->MinBitPool)
            {
                APPL_TRACE_WARNING("btif_media_task_enc_update computed bitpool too small (%d)", s16BitPool);

                /* Increase bitrate */
                UINT16 previous_u16BitRate = btif_media_cb.encoder.u16BitRate;
                btif_media_cb.encoder.u16BitRate += BTIF_MEDIA_BITRATE_STEP;
                /* Record that we have increased the bitrate */
                protect |= 2;
                /* Check over-flow */
                if (btif_media_cb.encoder.u16BitRate < previous_u16BitRate)
                    protect |= 3;
            }
            else
            {
                break;
            }
            /* In case we have already increased and decreased the bitrate, just stop */
            if (protect == 3)
            {
                APPL_TRACE_ERROR("btif_media_task_enc_update could not find bitpool in range");
                break;
            }
        } while (1);

        /* Finally update the bitpool in the encoder structure */
        pstrEncParams->s16BitPool = s16BitPool;

        APPL_TRACE_DEBUG("btif_media_task_enc_update final bit rate %d, final bit pool %d",
                btif_media_cb.encoder.u16BitRate, btif_media_cb.encoder.s16BitPool);

#ifndef BTA_AV_SPLIT_A2DP_ENABLED
        /* make sure we reinitialize encoder with new settings */
        SBC_Encoder_Init(&(btif_media_cb.encoder));
#endif
        btif_media_cb.TxNumSBCFrames = check_for_max_number_of_frames_per_packet();
    }
}

/*******************************************************************************
 **
 ** Function         btif_media_task_pcm2sbc_init
 **
 ** Description      Init encoding task for PCM to SBC according to feeding
 **
 ** Returns          void
 **
 *******************************************************************************/
static void btif_media_task_pcm2sbc_init(tBTIF_MEDIA_INIT_AUDIO_FEEDING * p_feeding)
{
    BOOLEAN reconfig_needed = FALSE;

    APPL_TRACE_DEBUG("PCM feeding:");
    APPL_TRACE_DEBUG("sampling_freq:%d", p_feeding->feeding.cfg.pcm.sampling_freq);
    APPL_TRACE_DEBUG("num_channel:%d", p_feeding->feeding.cfg.pcm.num_channel);
    APPL_TRACE_DEBUG("bit_per_sample:%d", p_feeding->feeding.cfg.pcm.bit_per_sample);

    /* Check the PCM feeding sampling_freq */
    switch (p_feeding->feeding.cfg.pcm.sampling_freq)
    {
        case  8000:
        case 12000:
        case 16000:
        case 24000:
        case 32000:
        case 48000:
            /* For these sampling_freq the AV connection must be 48000 */
            if (btif_media_cb.encoder.s16SamplingFreq != SBC_sf48000)
            {
                /* Reconfiguration needed at 48000 */
                APPL_TRACE_DEBUG("SBC Reconfiguration needed at 48000");
                btif_media_cb.encoder.s16SamplingFreq = SBC_sf48000;
                reconfig_needed = TRUE;
            }
            break;

        case 11025:
        case 22050:
        case 44100:
            /* For these sampling_freq the AV connection must be 44100 */
            if (btif_media_cb.encoder.s16SamplingFreq != SBC_sf44100)
            {
                /* Reconfiguration needed at 44100 */
                APPL_TRACE_DEBUG("SBC Reconfiguration needed at 44100");
                btif_media_cb.encoder.s16SamplingFreq = SBC_sf44100;
                reconfig_needed = TRUE;
            }
            break;
        default:
            APPL_TRACE_DEBUG("Feeding PCM sampling_freq unsupported");
            break;
    }

    /* Some AV Headsets do not support Mono => always ask for Stereo */
    if (btif_media_cb.encoder.s16ChannelMode == SBC_MONO)
    {
        APPL_TRACE_DEBUG("SBC Reconfiguration needed in Stereo");
        btif_media_cb.encoder.s16ChannelMode = SBC_JOINT_STEREO;
        reconfig_needed = TRUE;
    }

    if (reconfig_needed != FALSE)
    {
        APPL_TRACE_DEBUG("btif_media_task_pcm2sbc_init :: mtu %d", btif_media_cb.TxAaMtuSize);
        APPL_TRACE_DEBUG("ch mode %d, nbsubd %d, nb %d, alloc %d, rate %d, freq %d",
                btif_media_cb.encoder.s16ChannelMode,
                btif_media_cb.encoder.s16NumOfSubBands, btif_media_cb.encoder.s16NumOfBlocks,
                btif_media_cb.encoder.s16AllocationMethod, btif_media_cb.encoder.u16BitRate,
                btif_media_cb.encoder.s16SamplingFreq);
#ifndef BTA_AV_SPLIT_A2DP_ENABLED
        SBC_Encoder_Init(&(btif_media_cb.encoder));
#endif
    }
    else
    {
        APPL_TRACE_DEBUG("btif_media_task_pcm2sbc_init no SBC reconfig needed");
    }
}


/*******************************************************************************
 **
 ** Function         btif_media_task_audio_feeding_init
 **
 ** Description      Initialize the audio path according to the feeding format
 **
 ** Returns          void
 **
 *******************************************************************************/
static void btif_media_task_audio_feeding_init(BT_HDR *p_msg)
{
    tBTIF_MEDIA_INIT_AUDIO_FEEDING *p_feeding = (tBTIF_MEDIA_INIT_AUDIO_FEEDING *) p_msg;

    APPL_TRACE_DEBUG("btif_media_task_audio_feeding_init format:%d", p_feeding->feeding.format);

    /* Save Media Feeding information */
    btif_media_cb.feeding_mode = p_feeding->feeding_mode;
    btif_media_cb.media_feeding = p_feeding->feeding;

    /* Handle different feeding formats */
    switch (p_feeding->feeding.format)
    {
        case BTIF_AV_CODEC_PCM:
            btif_media_cb.TxTranscoding = BTIF_MEDIA_TRSCD_PCM_2_SBC;
            btif_media_task_pcm2sbc_init(p_feeding);
            break;

        default :
            APPL_TRACE_ERROR("unknown feeding format %d", p_feeding->feeding.format);
            break;
    }
}

int btif_a2dp_get_track_frequency(UINT8 frequency) {
    int freq = 48000;
    switch (frequency) {
        case A2D_SBC_IE_SAMP_FREQ_16:
            freq = 16000;
            break;
        case A2D_SBC_IE_SAMP_FREQ_32:
            freq = 32000;
            break;
        case A2D_SBC_IE_SAMP_FREQ_44:
            freq = 44100;
            break;
        case A2D_SBC_IE_SAMP_FREQ_48:
            freq = 48000;
            break;
    }
    return freq;
}

int btif_a2dp_get_track_channel_count(UINT8 channeltype) {
    int count = 1;
    switch (channeltype) {
        case A2D_SBC_IE_CH_MD_MONO:
            count = 1;
            break;
        case A2D_SBC_IE_CH_MD_DUAL:
        case A2D_SBC_IE_CH_MD_STEREO:
        case A2D_SBC_IE_CH_MD_JOINT:
            count = 2;
            break;
    }
    return count;
}
#ifdef USE_AUDIO_TRACK
    int a2dp_get_track_channel_type(UINT8 channeltype) {
        int count = 1;
        switch (channeltype) {
            case A2D_SBC_IE_CH_MD_MONO:
                count = 1;
                break;
            case A2D_SBC_IE_CH_MD_DUAL:
            case A2D_SBC_IE_CH_MD_STEREO:
            case A2D_SBC_IE_CH_MD_JOINT:
                count = 3;
                break;
        }
        return count;
    }
#endif

void btif_a2dp_set_peer_sep(UINT8 sep) {
    btif_media_cb.peer_sep = sep;
}

static void btif_decode_alarm_cb(UNUSED_ATTR void *context) {
    if(worker_thread != NULL)
        thread_post(worker_thread, btif_media_task_avk_handle_timer, NULL);
}

static void btif_media_task_aa_handle_stop_decoding(void) {
  alarm_free(btif_media_cb.decode_alarm);
  btif_media_cb.decode_alarm = NULL;
#ifdef USE_AUDIO_TRACK
  btPauseTrack();
#endif
}

static void btif_media_task_aa_handle_start_decoding(void) {
  if (btif_media_cb.decode_alarm)
    return;
#ifdef USE_AUDIO_TRACK
  btStartTrack();
#endif
  btif_media_cb.decode_alarm = alarm_new();
  if (!btif_media_cb.decode_alarm) {
    LOG_ERROR("%s unable to allocate decode alarm.", __func__);
    return;
  }

  alarm_set_periodic(btif_media_cb.decode_alarm, BTIF_SINK_MEDIA_TIME_TICK, btif_decode_alarm_cb, NULL);
}

#if (BTA_AV_SINK_INCLUDED == TRUE)

static void btif_media_task_aa_handle_clear_track (void)
{
    APPL_TRACE_DEBUG("btif_media_task_aa_handle_clear_track");
#ifdef USE_AUDIO_TRACK
    btStopTrack();
    btDeleteTrack();
#endif
}

/*******************************************************************************
 **
 ** Function         btif_media_task_aa_handle_decoder_reset
 **
 ** Description
 **
 ** Returns          void
 **
 *******************************************************************************/
static void btif_media_task_aa_handle_decoder_reset(BT_HDR *p_msg)
{
    tBTIF_MEDIA_SINK_CFG_UPDATE *p_buf = (tBTIF_MEDIA_SINK_CFG_UPDATE*) p_msg;
    tA2D_STATUS a2d_status;
    tA2D_SBC_CIE sbc_cie;
    OI_STATUS       status;
    UINT32          freq_multiple = 48*20; /* frequency multiple for 20ms of data , initialize with 48K*/
    UINT32          num_blocks = 16;
    UINT32          num_subbands = 8;

    APPL_TRACE_DEBUG("btif_media_task_aa_handle_decoder_reset p_codec_info[%x:%x:%x:%x:%x:%x]",
            p_buf->codec_info[1], p_buf->codec_info[2], p_buf->codec_info[3],
            p_buf->codec_info[4], p_buf->codec_info[5], p_buf->codec_info[6]);

    a2d_status = A2D_ParsSbcInfo(&sbc_cie, p_buf->codec_info, FALSE);
    if (a2d_status != A2D_SUCCESS)
    {
        APPL_TRACE_ERROR("ERROR dump_codec_info A2D_ParsSbcInfo fail:%d", a2d_status);
        return;
    }

    btif_media_cb.sample_rate = btif_a2dp_get_track_frequency(sbc_cie.samp_freq);
    btif_media_cb.channel_count = btif_a2dp_get_track_channel_count(sbc_cie.ch_mode);

    btif_media_cb.rx_flush = FALSE;
    APPL_TRACE_DEBUG("Reset to sink role");
    status = OI_CODEC_SBC_DecoderReset(&context, contextData, sizeof(contextData), 2, 2, FALSE);
    if (!OI_SUCCESS(status)) {
        APPL_TRACE_ERROR("OI_CODEC_SBC_DecoderReset failed with error code %d\n", status);
    }

#ifdef USE_AUDIO_TRACK
    APPL_TRACE_DEBUG("A2dpSink: sbc Create Track");
    if (btCreateTrack(btif_a2dp_get_track_frequency(sbc_cie.samp_freq), a2dp_get_track_channel_type(sbc_cie.ch_mode)) == -1) {
        APPL_TRACE_ERROR("A2dpSink: Track creation fails!!!");
        return;
    }
#else
    UIPC_Open(UIPC_CH_ID_AV_AUDIO, btif_a2dp_data_cb);
#endif

    switch(sbc_cie.samp_freq)
    {
        case A2D_SBC_IE_SAMP_FREQ_16:
            APPL_TRACE_DEBUG("\tsamp_freq:%d (16000)", sbc_cie.samp_freq);
            freq_multiple = 16*20;
            break;
        case A2D_SBC_IE_SAMP_FREQ_32:
            APPL_TRACE_DEBUG("\tsamp_freq:%d (32000)", sbc_cie.samp_freq);
            freq_multiple = 32*20;
            break;
        case A2D_SBC_IE_SAMP_FREQ_44:
            APPL_TRACE_DEBUG("\tsamp_freq:%d (44100)", sbc_cie.samp_freq);
            freq_multiple = 441*2;
            break;
        case A2D_SBC_IE_SAMP_FREQ_48:
            APPL_TRACE_DEBUG("\tsamp_freq:%d (48000)", sbc_cie.samp_freq);
            freq_multiple = 48*20;
            break;
        default:
            APPL_TRACE_DEBUG(" Unknown Frequency ");
            break;
    }

    switch(sbc_cie.ch_mode)
    {
        case A2D_SBC_IE_CH_MD_MONO:
            APPL_TRACE_DEBUG("\tch_mode:%d (Mono)", sbc_cie.ch_mode);
            break;
        case A2D_SBC_IE_CH_MD_DUAL:
            APPL_TRACE_DEBUG("\tch_mode:%d (DUAL)", sbc_cie.ch_mode);
            break;
        case A2D_SBC_IE_CH_MD_STEREO:
            APPL_TRACE_DEBUG("\tch_mode:%d (STEREO)", sbc_cie.ch_mode);
            break;
        case A2D_SBC_IE_CH_MD_JOINT:
            APPL_TRACE_DEBUG("\tch_mode:%d (JOINT)", sbc_cie.ch_mode);
            break;
        default:
            APPL_TRACE_DEBUG(" Unknown Mode ");
            break;
    }

    switch(sbc_cie.block_len)
    {
        case A2D_SBC_IE_BLOCKS_4:
            APPL_TRACE_DEBUG("\tblock_len:%d (4)", sbc_cie.block_len);
            num_blocks = 4;
            break;
        case A2D_SBC_IE_BLOCKS_8:
            APPL_TRACE_DEBUG("\tblock_len:%d (8)", sbc_cie.block_len);
            num_blocks = 8;
            break;
        case A2D_SBC_IE_BLOCKS_12:
            APPL_TRACE_DEBUG("\tblock_len:%d (12)", sbc_cie.block_len);
            num_blocks = 12;
            break;
        case A2D_SBC_IE_BLOCKS_16:
            APPL_TRACE_DEBUG("\tblock_len:%d (16)", sbc_cie.block_len);
            num_blocks = 16;
            break;
        default:
            APPL_TRACE_DEBUG(" Unknown BlockLen ");
            break;
    }

    switch(sbc_cie.num_subbands)
    {
        case A2D_SBC_IE_SUBBAND_4:
            APPL_TRACE_DEBUG("\tnum_subbands:%d (4)", sbc_cie.num_subbands);
            num_subbands = 4;
            break;
        case A2D_SBC_IE_SUBBAND_8:
            APPL_TRACE_DEBUG("\tnum_subbands:%d (8)", sbc_cie.num_subbands);
            num_subbands = 8;
            break;
        default:
            APPL_TRACE_DEBUG(" Unknown SubBands ");
            break;
    }

    switch(sbc_cie.alloc_mthd)
    {
        case A2D_SBC_IE_ALLOC_MD_S:
            APPL_TRACE_DEBUG("\talloc_mthd:%d (SNR)", sbc_cie.alloc_mthd);
            break;
        case A2D_SBC_IE_ALLOC_MD_L:
            APPL_TRACE_DEBUG("\talloc_mthd:%d (Loudness)", sbc_cie.alloc_mthd);
            break;
        default:
            APPL_TRACE_DEBUG(" Unknown Allocation Method");
            break;
    }

    APPL_TRACE_DEBUG("\tBit pool Min:%d Max:%d", sbc_cie.min_bitpool, sbc_cie.max_bitpool);

    btif_media_cb.frames_to_process = ((freq_multiple)/(num_blocks*num_subbands)) + 1;
    APPL_TRACE_DEBUG(" Frames to be processed in 20 ms %d",btif_media_cb.frames_to_process);
}
#endif

/*******************************************************************************
 **
 ** Function         btif_media_task_feeding_state_reset
 **
 ** Description      Reset the media feeding state
 **
 ** Returns          void
 **
 *******************************************************************************/
static void btif_media_task_feeding_state_reset(void)
{
    /* By default, just clear the entire state */
    memset(&btif_media_cb.media_feeding_state, 0, sizeof(btif_media_cb.media_feeding_state));

    if (btif_media_cb.TxTranscoding == BTIF_MEDIA_TRSCD_PCM_2_SBC)
    {
        btif_media_cb.media_feeding_state.pcm.bytes_per_tick =
                (btif_media_cb.media_feeding.cfg.pcm.sampling_freq *
                 btif_media_cb.media_feeding.cfg.pcm.bit_per_sample / 8 *
                 btif_media_cb.media_feeding.cfg.pcm.num_channel *
                 BTIF_MEDIA_TIME_TICK)/1000;

        APPL_TRACE_WARNING("pcm bytes per tick %d",
                            (int)btif_media_cb.media_feeding_state.pcm.bytes_per_tick);
    }
}

static void btif_media_task_alarm_cb(UNUSED_ATTR void *context) {
  thread_post(worker_thread, btif_media_task_aa_handle_timer, NULL);
}

/*******************************************************************************
 **
 ** Function         btif_media_task_aa_start_tx
 **
 ** Description      Start media task encoding
 **
 ** Returns          void
 **
 *******************************************************************************/
static void btif_media_task_aa_start_tx(void)
{
    APPL_TRACE_IMP("btif_media_task_aa_start_tx is timer %d, feeding mode %d",
             btif_media_cb.is_tx_timer, btif_media_cb.feeding_mode);

    /* Use a timer to poll the UIPC, get rid of the UIPC call back */
    // UIPC_Ioctl(UIPC_CH_ID_AV_AUDIO, UIPC_REG_CBACK, NULL);

    btif_media_cb.is_tx_timer = TRUE;
    last_frame_us = 0;

    /* Reset the media feeding state */
    btif_media_task_feeding_state_reset();

#ifndef BTA_AV_SPLIT_A2DP_ENABLED

    APPL_TRACE_EVENT("starting timer %dms", BTIF_MEDIA_TIME_TICK);

    assert(btif_media_cb.media_alarm == NULL);

    btif_media_cb.media_alarm = alarm_new();
    if (!btif_media_cb.media_alarm) {
      LOG_ERROR("%s unable to allocate media alarm.", __func__);
      return;
    }

    alarm_set_periodic(btif_media_cb.media_alarm, BTIF_MEDIA_TIME_TICK, btif_media_task_alarm_cb, NULL);
#endif
}

/*******************************************************************************
 **
 ** Function         btif_media_task_aa_stop_tx
 **
 ** Description      Stop media task encoding
 **
 ** Returns          void
 **
 *******************************************************************************/
static void btif_media_task_aa_stop_tx(void)
{
#ifndef BTA_AV_SPLIT_A2DP_ENABLED
    APPL_TRACE_IMP("%s is_tx_timer: %d", __func__, btif_media_cb.is_tx_timer);

    const bool send_ack = (btif_media_cb.is_tx_timer != FALSE);

    /* Stop the timer first */
    alarm_free(btif_media_cb.media_alarm);
    btif_media_cb.media_alarm = NULL;
    btif_media_cb.is_tx_timer = FALSE;

    UIPC_Close(UIPC_CH_ID_AV_AUDIO);

    /* Try to send acknowldegment once the media stream is
       stopped. This will make sure that the A2DP HAL layer is
       un-blocked on wait for acknowledgment for the sent command.
       This resolves a corner cases AVDTP SUSPEND collision
       when the DUT and the remote device issue SUSPEND simultaneously
       and due to the processing of the SUSPEND request from the remote,
       the media path is torn down. If the A2DP HAL happens to wait
       for ACK for the initiated SUSPEND, it would never receive it casuing
       a block/wait. Due to this acknowledgement, the A2DP HAL is guranteed
       to get the ACK for any pending command in such cases. */

    if (send_ack)
        a2dp_cmd_acknowledge(A2DP_CTRL_ACK_SUCCESS);

    /* audio engine stopped, reset tx suspended flag */
    btif_media_cb.tx_flush = 0;
    last_frame_us = 0;

    /* Reset the media feeding state */
    btif_media_task_feeding_state_reset();
#else
    APPL_TRACE_IMP("%s tx_started: %d, tx_stop_initiated: %d",
        __func__, btif_media_cb.tx_started, btif_media_cb.tx_stop_initiated);
    if (btif_media_cb.tx_started && !btif_media_cb.tx_stop_initiated)
        btif_media_send_vendor_stop();
    else
        a2dp_cmd_acknowledge(A2DP_CTRL_ACK_SUCCESS);
#endif
}

static UINT32 get_frame_length()
{
    UINT32 frame_len = 0;
    APPL_TRACE_DEBUG("channel mode: %d, sub-band: %d, number of block: %d, \
            bitpool: %d, sampling frequency: %d, num channels: %d",
            btif_media_cb.encoder.s16ChannelMode,
            btif_media_cb.encoder.s16NumOfSubBands,
            btif_media_cb.encoder.s16NumOfBlocks,
            btif_media_cb.encoder.s16BitPool,
            btif_media_cb.encoder.s16SamplingFreq,
            btif_media_cb.encoder.s16NumOfChannels);

    switch(btif_media_cb.encoder.s16ChannelMode)
    {
        case SBC_MONO:
        case SBC_DUAL:
            frame_len = 4 + ((UINT32)(4 * btif_media_cb.encoder.s16NumOfSubBands *
                btif_media_cb.encoder.s16NumOfChannels) / 8) +
                ((UINT32)(btif_media_cb.encoder.s16NumOfBlocks *
                btif_media_cb.encoder.s16NumOfChannels *
                btif_media_cb.encoder.s16BitPool) / 8);
            break;
        case SBC_STEREO:
            frame_len = 4 + ((UINT32)(4 * btif_media_cb.encoder.s16NumOfSubBands *
                btif_media_cb.encoder.s16NumOfChannels) / 8) +
                ((UINT32)(btif_media_cb.encoder.s16NumOfBlocks *
                btif_media_cb.encoder.s16BitPool) / 8);
            break;
        case SBC_JOINT_STEREO:
            frame_len = 4 + ((UINT32)(4 * btif_media_cb.encoder.s16NumOfSubBands *
                btif_media_cb.encoder.s16NumOfChannels) / 8) +
                ((UINT32)(btif_media_cb.encoder.s16NumOfSubBands +
                (btif_media_cb.encoder.s16NumOfBlocks *
                btif_media_cb.encoder.s16BitPool)) / 8);
            break;
        default:
            APPL_TRACE_DEBUG("Invalid channel number");
    }
    APPL_TRACE_DEBUG("calculated frame length: %d", frame_len);
    return frame_len;
}

static UINT8 check_for_max_number_of_frames_per_packet()
{
    UINT16 result = 0;
    UINT16 effective_mtu_size = btif_media_cb.TxAaMtuSize;
    UINT32 frame_len;

    APPL_TRACE_DEBUG("original AVDTP MTU size: %d", btif_media_cb.TxAaMtuSize);
    if (btif_av_is_peer_edr() && (btif_av_peer_supports_3mbps() == FALSE)) {
        // This condition would be satisfied only if remote is EDR and supports only 2mbps
        // but effective AVDTP MTU size exceeds 2dh5 packet size
        APPL_TRACE_DEBUG("Headset is edr but does not support 3mbps");
        if (effective_mtu_size > MAX_2MBPS_AVDTP_MTU)
        {
            APPL_TRACE_DEBUG("restricting AVDTP MTU size to 675");
            effective_mtu_size = MAX_2MBPS_AVDTP_MTU;
            btif_media_cb.TxAaMtuSize = effective_mtu_size;
        }
    }

    if (!btif_media_cb.encoder.s16NumOfSubBands)
    {
        APPL_TRACE_ERROR("Error: SubBands are set to 0, resetting to Max");
        btif_media_cb.encoder.s16NumOfSubBands = SBC_MAX_NUM_OF_SUBBANDS;
    }
    if (!btif_media_cb.encoder.s16NumOfBlocks)
    {
        APPL_TRACE_ERROR("Error: Blocks are set to 0, resetting to Max");
        btif_media_cb.encoder.s16NumOfBlocks = SBC_MAX_NUM_OF_BLOCKS;
    }
    if (!btif_media_cb.encoder.s16NumOfChannels)
    {
        APPL_TRACE_ERROR("Error: Channels are set to 0, resetting to Max");
        btif_media_cb.encoder.s16NumOfChannels = SBC_MAX_NUM_OF_CHANNELS;
    }
    frame_len = get_frame_length();

    APPL_TRACE_DEBUG("effective Tx MTU to be considered: %d",
                                            effective_mtu_size);
    switch(btif_media_cb.encoder.s16SamplingFreq)
    {
        case SBC_sf44100:
            if(!frame_len)
            {
                APPL_TRACE_ERROR("Error: Calculating frame length, \
                                            resetting it to default");
                frame_len = MAX_SBC_HQ_FRAME_SIZE_44_1;
            }
            result = (effective_mtu_size - A2DP_HDR_SIZE) / frame_len;
            APPL_TRACE_DEBUG("max number of sbc frames: %d", result);
            break;

        case SBC_sf48000:
            if(!frame_len)
            {
                APPL_TRACE_ERROR("Error: Calculating frame length, \
                                            resetting it to default");
                frame_len = MAX_SBC_HQ_FRAME_SIZE_48;
            }
            result = (effective_mtu_size - A2DP_HDR_SIZE) / frame_len;
            APPL_TRACE_DEBUG("max number of sbc frames: %d", result);
            break;

        default:
            APPL_TRACE_ERROR("Error: max number of sbc frames: %d", result);

    }
    return result;
}

/*******************************************************************************
 **
 ** Function         btif_get_num_aa_frame
 **
 ** Description      returns number of frames to send and number of iterations
 **                  to be used. num_of_ietrations and num_of_frames parameters
 **                  are used as output param for returning the respective values
 **
 ** Returns          void
 **
 *******************************************************************************/
static void btif_get_num_aa_frame(UINT8 *num_of_iterations, UINT8 *num_of_frames)
{
    UINT32 result=0;
    UINT8 nof = 0;
    UINT8 noi = 1;

    switch (btif_media_cb.TxTranscoding)
    {
        case BTIF_MEDIA_TRSCD_PCM_2_SBC:
        {
            UINT32 pcm_bytes_per_frame = btif_media_cb.encoder.s16NumOfSubBands *
                             btif_media_cb.encoder.s16NumOfBlocks *
                             btif_media_cb.media_feeding.cfg.pcm.num_channel *
                             btif_media_cb.media_feeding.cfg.pcm.bit_per_sample / 8;
            APPL_TRACE_DEBUG("pcm_bytes_per_frame %u", pcm_bytes_per_frame);

            UINT32 us_this_tick = BTIF_MEDIA_TIME_TICK * 1000;
            UINT64 now_us = time_now_us();
            if (last_frame_us != 0)
                us_this_tick = (now_us - last_frame_us);
            last_frame_us = now_us;

            btif_media_cb.media_feeding_state.pcm.counter +=
                                btif_media_cb.media_feeding_state.pcm.bytes_per_tick *
                                us_this_tick / (BTIF_MEDIA_TIME_TICK * 1000);

            /* calculate nbr of frames pending for this media tick */
            result = btif_media_cb.media_feeding_state.pcm.counter/pcm_bytes_per_frame;
            APPL_TRACE_DEBUG("num of frames calculated as per available pcm data:  %u", result);
            if(btif_av_is_peer_edr())
            {
                if (!btif_media_cb.TxNumSBCFrames)
                {
                    APPL_TRACE_ERROR("Error: TxNumSBCFrames not updated, update from here");
                    btif_media_cb.TxNumSBCFrames = check_for_max_number_of_frames_per_packet();
                }
                nof = btif_media_cb.TxNumSBCFrames;
                if(!nof) {
                    APPL_TRACE_ERROR("Error: Num frames not updated, set calculated values");
                    nof = result;
                    noi = 1;
                }
                else
                {
                    if (nof < result)
                    {
                        noi = result / nof; // number of iterations would vary
                        if (noi > MAX_PCM_ITER_NUM_PER_TICK)
                        {
                            APPL_TRACE_ERROR("## Audio Congestion (iterations:%d > max (%d))",
                                 noi, MAX_PCM_ITER_NUM_PER_TICK);
                            noi = MAX_PCM_ITER_NUM_PER_TICK;
                            btif_media_cb.media_feeding_state.pcm.counter
                                = noi * nof * pcm_bytes_per_frame;
                        }
                        result = nof;
                    }
                    else
                    {
                        noi = 1; // number of iterations is 1
                        APPL_TRACE_DEBUG("reducing number of frames as per available pcm data");
                        nof = result;
                    }
                }
            }
            else
            {
                // For BR cases nof will be same as the value retrieved at result
                APPL_TRACE_DEBUG("headset is of type BR %u", nof);
                if (result > MAX_PCM_FRAME_NUM_PER_TICK)
                {
                    APPL_TRACE_ERROR("## Audio Congestion (frames: %d > max (%d))"
                        ,result, MAX_PCM_FRAME_NUM_PER_TICK);
                    result = MAX_PCM_FRAME_NUM_PER_TICK;
                    btif_media_cb.media_feeding_state.pcm.counter
                         = noi * result * pcm_bytes_per_frame;
                }
                nof = result;
            }
            btif_media_cb.media_feeding_state.pcm.counter -= noi * nof * pcm_bytes_per_frame;
            APPL_TRACE_DEBUG("effective num of frames %u", nof);
            APPL_TRACE_DEBUG("num of iterations %u", noi);
            LOG_VERBOSE("WRITE %d FRAMES", result);
        }
        break;

        default:
            APPL_TRACE_ERROR("ERROR btif_get_num_aa_frame Unsupported transcoding format 0x%x",
                    btif_media_cb.TxTranscoding);
            result = nof = 0;
            noi = 0;
            break;
    }
    *num_of_frames = nof;
    *num_of_iterations = noi;
}

/*******************************************************************************
 **
 ** Function         btif_media_sink_enque_buf
 **
 ** Description      This function is called by the av_co to fill A2DP Sink Queue
 **
 **
 ** Returns          size of the queue
 *******************************************************************************/
UINT8 btif_media_sink_enque_buf(BT_HDR *p_pkt)
{
    tBT_SBC_HDR *p_msg;

    if(btif_media_cb.rx_flush == TRUE) /* Flush enabled, do not enque*/
        return GKI_queue_length(&btif_media_cb.RxSbcQ);
    if(GKI_queue_length(&btif_media_cb.RxSbcQ) >= MAX_OUTPUT_A2DP_FRAME_QUEUE_SZ)
    {
         return GKI_queue_length(&btif_media_cb.RxSbcQ);
    }

    BTIF_TRACE_VERBOSE("btif_media_sink_enque_buf + ");
    /* allocate and Queue this buffer */
    if ((p_msg = (tBT_SBC_HDR *)GKI_getbuf(sizeof(tBT_SBC_HDR) + p_pkt->len)) != NULL)
    {
        UINT8 *p_dest;

        p_dest = (UINT8*)(p_msg + 1);
        memcpy(p_dest, (UINT8*)(p_pkt + 1) + p_pkt->offset, p_pkt->len);

        p_msg->num_frames_to_be_processed = p_dest[0] & 0x0f;
        p_msg->len = p_pkt->len;
        p_msg->offset = 0;
        p_msg->layer_specific = p_pkt->layer_specific;

        BTIF_TRACE_VERBOSE("btif_media_sink_enque_buf + ", p_msg->num_frames_to_be_processed);
        GKI_enqueue(&(btif_media_cb.RxSbcQ), p_msg);
        if(GKI_queue_length(&btif_media_cb.RxSbcQ) == MAX_A2DP_DELAYED_START_FRAME_COUNT)
        {
            BTIF_TRACE_DEBUG(" Initiate Decoding ");
            btif_media_task_aa_handle_start_decoding();
        }
    }
    else
    {
        /* let caller deal with a failed allocation */
        BTIF_TRACE_VERBOSE("btif_media_sink_enque_buf No Buffer left - ");
    }
    return GKI_queue_length(&btif_media_cb.RxSbcQ);
}

/*******************************************************************************
 **
 ** Function         btif_media_aa_readbuf
 **
 ** Description      This function is called by the av_co to get the next buffer to send
 **
 **
 ** Returns          void
 *******************************************************************************/
BT_HDR *btif_media_aa_readbuf(void)
{
    return GKI_dequeue(&(btif_media_cb.TxAaQ));
}

/*******************************************************************************
 **
 ** Function         btif_media_aa_read_feeding
 **
 ** Description
 **
 ** Returns          void
 **
 *******************************************************************************/

BOOLEAN btif_media_aa_read_feeding(tUIPC_CH_ID channel_id)
{
    UINT16 event;
    UINT16 blocm_x_subband = btif_media_cb.encoder.s16NumOfSubBands * \
                             btif_media_cb.encoder.s16NumOfBlocks;
    UINT32 read_size;
    UINT16 sbc_sampling = 48000;
    UINT32 src_samples;
    UINT16 bytes_needed = blocm_x_subband * btif_media_cb.encoder.s16NumOfChannels * \
                          btif_media_cb.media_feeding.cfg.pcm.bit_per_sample / 8;
    static UINT16 up_sampled_buffer[SBC_MAX_NUM_FRAME * SBC_MAX_NUM_OF_BLOCKS
            * SBC_MAX_NUM_OF_CHANNELS * SBC_MAX_NUM_OF_SUBBANDS * 2];
    static UINT16 read_buffer[SBC_MAX_NUM_FRAME * SBC_MAX_NUM_OF_BLOCKS
            * SBC_MAX_NUM_OF_CHANNELS * SBC_MAX_NUM_OF_SUBBANDS];
    UINT32 src_size_used;
    UINT32 dst_size_used;
    BOOLEAN fract_needed;
    INT32   fract_max;
    INT32   fract_threshold;
    UINT32  nb_byte_read;
    #ifdef BT_AUDIO_SYSTRACE_LOG
    char trace_buf[512];
    #endif

    /* Get the SBC sampling rate */
    switch (btif_media_cb.encoder.s16SamplingFreq)
    {
    case SBC_sf48000:
        sbc_sampling = 48000;
        break;
    case SBC_sf44100:
        sbc_sampling = 44100;
        break;
    case SBC_sf32000:
        sbc_sampling = 32000;
        break;
    case SBC_sf16000:
        sbc_sampling = 16000;
        break;
    }

    if (sbc_sampling == btif_media_cb.media_feeding.cfg.pcm.sampling_freq) {
        read_size = bytes_needed - btif_media_cb.media_feeding_state.pcm.aa_feed_residue;
        nb_byte_read = UIPC_Read(channel_id, &event,
                  ((UINT8 *)btif_media_cb.encoder.as16PcmBuffer) +
                  btif_media_cb.media_feeding_state.pcm.aa_feed_residue,
                  read_size);
        if (nb_byte_read == read_size) {
            btif_media_cb.media_feeding_state.pcm.aa_feed_residue = 0;
            return TRUE;
        } else {
            APPL_TRACE_WARNING("### UNDERFLOW :: ONLY READ %d BYTES OUT OF %d ###",
                nb_byte_read, read_size);
            btif_media_cb.media_feeding_state.pcm.aa_feed_residue += nb_byte_read;
            return FALSE;
        }
    }

    /* Some Feeding PCM frequencies require to split the number of sample */
    /* to read. */
    /* E.g 128/6=21.3333 => read 22 and 21 and 21 => max = 2; threshold = 0*/
    fract_needed = FALSE;   /* Default */
    switch (btif_media_cb.media_feeding.cfg.pcm.sampling_freq)
    {
    case 32000:
    case 8000:
        fract_needed = TRUE;
        fract_max = 2;          /* 0, 1 and 2 */
        fract_threshold = 0;    /* Add one for the first */
        break;
    case 16000:
        fract_needed = TRUE;
        fract_max = 2;          /* 0, 1 and 2 */
        fract_threshold = 1;    /* Add one for the first two frames*/
        break;
    }

    /* Compute number of sample to read from source */
    src_samples = blocm_x_subband;
    src_samples *= btif_media_cb.media_feeding.cfg.pcm.sampling_freq;
    src_samples /= sbc_sampling;

    /* The previous division may have a remainder not null */
    if (fract_needed)
    {
        if (btif_media_cb.media_feeding_state.pcm.aa_feed_counter <= fract_threshold)
        {
            src_samples++; /* for every read before threshold add one sample */
        }

        /* do nothing if counter >= threshold */
        btif_media_cb.media_feeding_state.pcm.aa_feed_counter++; /* one more read */
        if (btif_media_cb.media_feeding_state.pcm.aa_feed_counter > fract_max)
        {
            btif_media_cb.media_feeding_state.pcm.aa_feed_counter = 0;
        }
    }

    /* Compute number of bytes to read from source */
    read_size = src_samples;
    read_size *= btif_media_cb.media_feeding.cfg.pcm.num_channel;
    read_size *= (btif_media_cb.media_feeding.cfg.pcm.bit_per_sample / 8);

    /* Read Data from UIPC channel */
    nb_byte_read = UIPC_Read(channel_id, &event, (UINT8 *)read_buffer, read_size);

    //tput_mon(TRUE, nb_byte_read, FALSE);

    if (nb_byte_read < read_size)
    {
        APPL_TRACE_WARNING("### UNDERRUN :: ONLY READ %d BYTES OUT OF %d ###",
                nb_byte_read, read_size);
        #ifdef BT_AUDIO_SYSTRACE_LOG
        snprintf(trace_buf, 32, "A2DP UNDERRUN read %ld ", nb_byte_read);

        if (PERF_SYSTRACE)
        {
            ATRACE_BEGIN(trace_buf);
        }

        if (PERF_SYSTRACE)
        {
            ATRACE_END();
        }
        #endif

        if (nb_byte_read == 0)
            return FALSE;

        if(btif_media_cb.feeding_mode == BTIF_AV_FEEDING_ASYNCHRONOUS)
        {
            /* Fill the unfilled part of the read buffer with silence (0) */
            memset(((UINT8 *)read_buffer) + nb_byte_read, 0, read_size - nb_byte_read);
            nb_byte_read = read_size;
        }
    }

    /* Initialize PCM up-sampling engine */
    bta_av_sbc_init_up_sample(btif_media_cb.media_feeding.cfg.pcm.sampling_freq,
            sbc_sampling, btif_media_cb.media_feeding.cfg.pcm.bit_per_sample,
            btif_media_cb.media_feeding.cfg.pcm.num_channel);

    /* re-sample read buffer */
    /* The output PCM buffer will be stereo, 16 bit per sample */
    dst_size_used = bta_av_sbc_up_sample((UINT8 *)read_buffer,
            (UINT8 *)up_sampled_buffer + btif_media_cb.media_feeding_state.pcm.aa_feed_residue,
            nb_byte_read,
            sizeof(up_sampled_buffer) - btif_media_cb.media_feeding_state.pcm.aa_feed_residue,
            &src_size_used);

#if (defined(DEBUG_MEDIA_AV_FLOW) && (DEBUG_MEDIA_AV_FLOW == TRUE))
    APPL_TRACE_DEBUG("btif_media_aa_read_feeding readsz:%d src_size_used:%d dst_size_used:%d",
            read_size, src_size_used, dst_size_used);
#endif

    /* update the residue */
    btif_media_cb.media_feeding_state.pcm.aa_feed_residue += dst_size_used;

    /* only copy the pcm sample when we have up-sampled enough PCM */
    if(btif_media_cb.media_feeding_state.pcm.aa_feed_residue >= bytes_needed)
    {
        /* Copy the output pcm samples in SBC encoding buffer */
        memcpy((UINT8 *)btif_media_cb.encoder.as16PcmBuffer,
                (UINT8 *)up_sampled_buffer,
                bytes_needed);
        /* update the residue */
        btif_media_cb.media_feeding_state.pcm.aa_feed_residue -= bytes_needed;

        if (btif_media_cb.media_feeding_state.pcm.aa_feed_residue != 0)
        {
            memcpy((UINT8 *)up_sampled_buffer,
                   (UINT8 *)up_sampled_buffer + bytes_needed,
                   btif_media_cb.media_feeding_state.pcm.aa_feed_residue);
        }
        return TRUE;
    }

#if (defined(DEBUG_MEDIA_AV_FLOW) && (DEBUG_MEDIA_AV_FLOW == TRUE))
    APPL_TRACE_DEBUG("btif_media_aa_read_feeding residue:%d, dst_size_used %d, bytes_needed %d",
            btif_media_cb.media_feeding_state.pcm.aa_feed_residue, dst_size_used, bytes_needed);
#endif

    return FALSE;
}

/*******************************************************************************
 **
 ** Function         btif_media_aa_prep_sbc_2_send
 **
 ** Description
 **
 ** Returns          void
 **
 *******************************************************************************/
static void btif_media_aa_prep_sbc_2_send(UINT8 nb_frame)
{
    BT_HDR * p_buf;
    UINT16 blocm_x_subband = btif_media_cb.encoder.s16NumOfSubBands *
                             btif_media_cb.encoder.s16NumOfBlocks;

#if (defined(DEBUG_MEDIA_AV_FLOW) && (DEBUG_MEDIA_AV_FLOW == TRUE))
    APPL_TRACE_DEBUG("btif_media_aa_prep_sbc_2_send nb_frame %d, TxAaQ %d",
                       nb_frame, btif_media_cb.TxAaQ.count);
#endif
    while (nb_frame)
    {
        if (NULL == (p_buf = GKI_getpoolbuf(BTIF_MEDIA_AA_POOL_ID)))
        {
            APPL_TRACE_ERROR ("ERROR btif_media_aa_prep_sbc_2_send no buffer TxCnt %d ",
                                GKI_queue_length(&btif_media_cb.TxAaQ));
            return;
        }

        /* Init buffer */
        p_buf->offset = BTIF_MEDIA_AA_SBC_OFFSET;
        p_buf->len = 0;
        p_buf->layer_specific = 0;

        do
        {
            /* Write @ of allocated buffer in encoder.pu8Packet */
            btif_media_cb.encoder.pu8Packet = (UINT8 *) (p_buf + 1) + p_buf->offset + p_buf->len;
            /* Fill allocated buffer with 0 */
            memset(btif_media_cb.encoder.as16PcmBuffer, 0, blocm_x_subband
                    * btif_media_cb.encoder.s16NumOfChannels);

            /* Read PCM data and upsample them if needed */
            if (btif_media_aa_read_feeding(UIPC_CH_ID_AV_AUDIO))
            {
                /* SBC encode and descramble frame */
                SBC_Encoder(&(btif_media_cb.encoder));
                A2D_SbcChkFrInit(btif_media_cb.encoder.pu8Packet);
                A2D_SbcDescramble(btif_media_cb.encoder.pu8Packet, btif_media_cb.encoder.u16PacketLength);
                /* Update SBC frame length */
                p_buf->len += btif_media_cb.encoder.u16PacketLength;
                nb_frame--;
                p_buf->layer_specific++;
            }
            else
            {
                APPL_TRACE_WARNING("btif_media_aa_prep_sbc_2_send underflow %d, %d",
                    nb_frame, btif_media_cb.media_feeding_state.pcm.aa_feed_residue);
                btif_media_cb.media_feeding_state.pcm.counter += nb_frame *
                     btif_media_cb.encoder.s16NumOfSubBands *
                     btif_media_cb.encoder.s16NumOfBlocks *
                     btif_media_cb.media_feeding.cfg.pcm.num_channel *
                     btif_media_cb.media_feeding.cfg.pcm.bit_per_sample / 8;
                /* no more pcm to read */
                nb_frame = 0;

                /* break read loop if timer was stopped (media task stopped) */
                if ( btif_media_cb.is_tx_timer == FALSE )
                {
                    GKI_freebuf(p_buf);
                    return;
                }
            }

        } while (((p_buf->len + btif_media_cb.encoder.u16PacketLength) < btif_media_cb.TxAaMtuSize)
                && (p_buf->layer_specific < 0x0F) && nb_frame);

        if(p_buf->len)
        {
            /* timestamp of the media packet header represent the TS of the first SBC frame
               i.e the timestamp before including this frame */
            *((UINT32 *) (p_buf + 1)) = btif_media_cb.timestamp;

            btif_media_cb.timestamp += p_buf->layer_specific * blocm_x_subband;

            if (btif_media_cb.tx_flush)
            {
                APPL_TRACE_DEBUG("### tx suspended, discarded frame ###");

                if (GKI_queue_length(&btif_media_cb.TxAaQ) > 0)
                    btif_media_flush_q(&(btif_media_cb.TxAaQ));

                GKI_freebuf(p_buf);
                return;
            }

            /* Enqueue the encoded SBC frame in AA Tx Queue */
            GKI_enqueue(&(btif_media_cb.TxAaQ), p_buf);
        }
        else
        {
            GKI_freebuf(p_buf);
        }
    }
}


/*******************************************************************************
 **
 ** Function         btif_media_aa_prep_2_send
 **
 ** Description
 **
 ** Returns          void
 **
 *******************************************************************************/

static void btif_media_aa_prep_2_send(UINT8 nb_frame)
{
    UINT8* p_buf;

    // Check for TX queue overflow

    if (nb_frame > MAX_OUTPUT_A2DP_FRAME_QUEUE_SZ)
        nb_frame = MAX_OUTPUT_A2DP_FRAME_QUEUE_SZ;

    if (GKI_queue_length(&btif_media_cb.TxAaQ) > (MAX_OUTPUT_A2DP_FRAME_QUEUE_SZ - nb_frame))
    {
        APPL_TRACE_WARNING("%s() - TX queue buffer count %d/%d", __func__,
            GKI_queue_length(&btif_media_cb.TxAaQ), MAX_OUTPUT_A2DP_FRAME_QUEUE_SZ - nb_frame);
    }

    while (GKI_queue_length(&btif_media_cb.TxAaQ) > (MAX_OUTPUT_A2DP_FRAME_QUEUE_SZ - nb_frame))
    {
        p_buf = GKI_dequeue(&(btif_media_cb.TxAaQ));
        if (p_buf)
        {
            GKI_freebuf(p_buf);
        }
        else {
            APPL_TRACE_DEBUG("%s btif_media_cb.TxAaQ become empty", __func__);
            break;
        }
    }


    // Transcode frame

    switch (btif_media_cb.TxTranscoding)
    {
    case BTIF_MEDIA_TRSCD_PCM_2_SBC:
        btif_media_aa_prep_sbc_2_send(nb_frame);
        break;

    default:
        APPL_TRACE_ERROR("%s unsupported transcoding format 0x%x", __func__, btif_media_cb.TxTranscoding);
        break;
    }
}

/*******************************************************************************
 **
 ** Function         btif_media_send_aa_frame
 **
 ** Description
 **
 ** Returns          void
 **
 *******************************************************************************/
static void btif_media_send_aa_frame(void)
{
    UINT8 nb_frame_2_send;
    UINT8 nb_iterations;
    UINT8 counter;

    #ifdef BT_AUDIO_SYSTRACE_LOG
    char trace_buf[1024];
    #endif
    /* get the number of frame to send */
    btif_get_num_aa_frame(&nb_iterations, &nb_frame_2_send);

    for (counter = 0; counter < nb_iterations; counter++)
    {
        /* format and Q buffer to send */
        if (nb_frame_2_send != 0) {
            btif_media_aa_prep_2_send(nb_frame_2_send);
        }
    }
    /* send it */
    LOG_VERBOSE("btif_media_send_aa_frame : send %d frames", nb_frame_2_send);
    #ifdef BT_AUDIO_SYSTRACE_LOG
    snprintf(trace_buf, 32, "btif_media_send_aa_frame:");
    if (PERF_SYSTRACE)
    {
        ATRACE_BEGIN(trace_buf);
    }
    #endif

    /* send it */

    #ifdef BT_AUDIO_SYSTRACE_LOG
    if (PERF_SYSTRACE)
    {
        ATRACE_END();
    }
    #endif
    bta_av_ci_src_data_ready(BTA_AV_CHNL_AUDIO);
}

#ifdef BTA_AV_SPLIT_A2DP_ENABLED
/*******************************************************************************
 **
 ** Function         bta_av_co_send_vendor_start
 **
 ** Description      Send Vendor Specific A2dp START command to controller
 **
 ** Returns          TRUE if command succeeds, FALSE otherwize
 **
 *******************************************************************************/

#define HCI_VSQC_CONTROLLER_A2DP_OPCODE 0x000A

#define VS_QHCI_READ_A2DP_CFG                 0x01
#define VS_QHCI_WRITE_SBC_CFG                 0x02
#define VS_QHCI_WRITE_A2DP_MEDIA_CHANNEL_CFG  0x03
#define VS_QHCI_START_A2DP_MEDIA              0x04
#define VS_QHCI_STOP_A2DP_MEDIA               0x05
#define VS_QHCI_A2DP_WRITE_SUGGESTED_BITRATE  0x06
#define VS_QHCI_A2DP_TRANSPORT_CONFIGURATION  0x07
#define VS_QHCI_A2DP_WRITE_SCMS_T_CP          0x08

void btif_media_send_reset_vendor_state()
{
    BT_HDR *p_buf;
    if (NULL == (p_buf = GKI_getbuf(sizeof(BT_HDR))))
    {
        APPL_TRACE_EVENT("GKI alloc failed");
        return;
    }
    p_buf->event = BTIF_MEDIA_RESET_VS_STATE;
    fixed_queue_enqueue(btif_media_cmd_msg_queue, p_buf);
}

void btif_media_start_vendor_command()
{
    APPL_TRACE_IMP("btif_media_start_vendor_command_exchange:\
        vs_configs_exchanged:%u", btif_media_cb.vs_configs_exchanged);
    if(btif_media_cb.vs_configs_exchanged)
    {
        btif_media_send_vendor_start();
    }
    else
    {
        btif_media_send_vendor_write_sbc_cfg();
    }
}

void btif_media_on_start_vendor_command()
{
    BT_HDR *p_buf;
    if (NULL == (p_buf = GKI_getbuf(sizeof(BT_HDR))))
    {
        APPL_TRACE_EVENT("GKI alloc failed: ACK failure");
        a2dp_cmd_acknowledge(A2DP_CTRL_ACK_FAILURE);
        return;
    }
    p_buf->event = BTIF_MEDIA_START_VS_CMD;
    fixed_queue_enqueue(btif_media_cmd_msg_queue, p_buf);
}

void btif_media_on_stop_vendor_command()
{
    BT_HDR *p_buf;
    if (NULL == (p_buf = GKI_getbuf(sizeof(BT_HDR))))
    {
        APPL_TRACE_EVENT("GKI alloc failed: ACK failure");
        a2dp_cmd_acknowledge(A2DP_CTRL_ACK_FAILURE);
        return;
    }
    APPL_TRACE_IMP("btif_media_on_stop_vendor_command");
    p_buf->event = BTIF_MEDIA_STOP_VS_CMD;
    fixed_queue_enqueue(btif_media_cmd_msg_queue, p_buf);
}

void btif_media_a2dp_start_cb(tBTM_VSC_CMPL *param)
{
    unsigned char status = 0;
    BT_HDR *p_buf;

    if (param->param_len)
    {
        status = param->p_param_buf[0];
    }
    APPL_TRACE_IMP("VS_QHCI_START_A2DP_MEDIA sent with error code: %u", status);

    if ((!status) && (NULL != (p_buf = GKI_getbuf(sizeof(BT_HDR)))))
    {
        p_buf->event = BTIF_MEDIA_VS_A2DP_START_SUCCESS;
        fixed_queue_enqueue(btif_media_cmd_msg_queue, p_buf);
    }
    else
    {
        APPL_TRACE_ERROR("Error in processing Vendor command response");
        a2dp_cmd_acknowledge(A2DP_CTRL_ACK_FAILURE);
    }
}

BOOLEAN btif_media_send_vendor_start()
{
    UINT8 param[2];

    APPL_TRACE_IMP("btif_media_send_vendor_start");

    param[0] = VS_QHCI_START_A2DP_MEDIA;
    param[1] = 0; /*needs to send index for multi A2dp*/

    return BTA_DmVendorSpecificCommand(HCI_VSQC_CONTROLLER_A2DP_OPCODE, 2,
                                            param, btif_media_a2dp_start_cb);
}

void btif_media_a2dp_stop_cb(tBTM_VSC_CMPL *param)
{
    unsigned char status = 0;
    BT_HDR *p_buf;

    if (param->param_len)
    {
        status = param->p_param_buf[0];
    }
    APPL_TRACE_IMP("VS_QHCI_STOP_A2DP_MEDIA sent with error code: %u", status);

    if ((!status) && (NULL != (p_buf = GKI_getbuf(sizeof(BT_HDR)))))
        p_buf->event = BTIF_MEDIA_VS_A2DP_STOP_SUCCESS;
    else
        p_buf->event = BTIF_MEDIA_VS_A2DP_STOP_FAILURE;

    fixed_queue_enqueue(btif_media_cmd_msg_queue, p_buf);
}

BOOLEAN btif_media_send_vendor_stop()
{
    UINT8 param[2];

    APPL_TRACE_IMP("btif_media_send_vendor_stop");

    btif_media_cb.tx_stop_initiated = TRUE;

    param[0] = VS_QHCI_STOP_A2DP_MEDIA;
    param[1] = 0; /*needs to send index for multi A2dp*/

    return BTA_DmVendorSpecificCommand(HCI_VSQC_CONTROLLER_A2DP_OPCODE, 2,
                                            param, btif_media_a2dp_stop_cb);
}

void btif_media_a2dp_media_chn_cfg_cb(tBTM_VSC_CMPL *param)
{
    unsigned char status = 0;
    BT_HDR *p_buf;

    if (param->param_len)
    {
        status = param->p_param_buf[0];
    }
    APPL_TRACE_IMP("VS_QHCI_WRITE_A2DP_MEDIA_CHANNEL_CFG sent with error code: %u",
                                                                        status);

    if ((!status) && (NULL != (p_buf = GKI_getbuf(sizeof(BT_HDR)))))
    {
        p_buf->event = BTIF_MEDIA_VS_A2DP_MEDIA_CHNL_CFG_SUCCESS;
        fixed_queue_enqueue(btif_media_cmd_msg_queue, p_buf);
    }
    else
    {
        APPL_TRACE_ERROR("Error in processing Vendor command response");
        a2dp_cmd_acknowledge(A2DP_CTRL_ACK_FAILURE);
    }
}

BOOLEAN btif_media_send_vendor_media_chn_cfg()
{
    UINT8 param[8];
    bt_bdaddr_t bd_addr;
    BD_ADDR addr;
    btif_av_get_peer_addr(&bd_addr);
    memcpy(addr, bd_addr.address, sizeof(BD_ADDR));
    UINT16 acl_hdl = BTM_GetHCIConnHandle(addr, BT_TRANSPORT_BR_EDR);
    APPL_TRACE_IMP("btif_media_send_vendor_media_chn_cfg");
    APPL_TRACE_IMP("AVDTP mtu: %u, hdl: %u", btif_media_cb.TxAaMtuSize, acl_hdl);

    param[0] = VS_QHCI_WRITE_A2DP_MEDIA_CHANNEL_CFG;
    param[1] = 0; /*needs to send index for multi A2dp*/
    param[2] = (UINT8)(acl_hdl & 0x00ff);
    param[3] = (UINT8)(((acl_hdl & 0xff00) >> 8) & 0x00ff);
    param[4] = (UINT8)(btif_av_get_streaming_channel_id()& 0x00ff);
    param[5] = (UINT8)(((btif_av_get_streaming_channel_id() & 0xff00)
                                                        >> 8) & 0x00ff);
    param[6] = (UINT8)(btif_media_cb.TxAaMtuSize & 0x00ff);
    param[7] = (UINT8)(((btif_media_cb.TxAaMtuSize & 0xff00) >> 8) & 0x00ff);
    return BTA_DmVendorSpecificCommand(HCI_VSQC_CONTROLLER_A2DP_OPCODE, 8,
                                    param, btif_media_a2dp_media_chn_cfg_cb);
}

void btif_media_a2dp_write_sbc_cfg_cb(tBTM_VSC_CMPL *param)
{
    unsigned char status = 0;
    BT_HDR *p_buf;

    if (param->param_len)
    {
        status = param->p_param_buf[0];
    }
    APPL_TRACE_IMP("VS_QHCI_WRITE_SBC_CFG sent with error code: %u", status);

    if ((!status) && (NULL != (p_buf = GKI_getbuf(sizeof(BT_HDR)))))
    {
        p_buf->event = BTIF_MEDIA_VS_A2DP_WRITE_SBC_CFG_SUCCESS;
        fixed_queue_enqueue(btif_media_cmd_msg_queue, p_buf);
    }
    else
    {
        APPL_TRACE_ERROR("Error in processing Vendor command response");
        a2dp_cmd_acknowledge(A2DP_CTRL_ACK_FAILURE);
    }
}

BOOLEAN btif_media_send_vendor_write_sbc_cfg()
{
    UINT8 param[12];
    bt_bdaddr_t bd_addr;
    BD_ADDR addr;
    btif_av_get_peer_addr(&bd_addr);
    memcpy(addr, bd_addr.address, sizeof(BD_ADDR));
    UINT16 acl_hdl = BTM_GetHCIConnHandle(addr, BT_TRANSPORT_BR_EDR);
    APPL_TRACE_IMP("btif_media_send_vendor_write_sbc_cfg");
    APPL_TRACE_IMP("acl hdl: %u", acl_hdl);
    APPL_TRACE_IMP("channel mode: %u", btif_media_cb.encoder.s16ChannelMode);
    APPL_TRACE_IMP("sampling frequency: %u", btif_media_cb.encoder.s16SamplingFreq);
    APPL_TRACE_IMP("allocation method: %u", btif_media_cb.encoder.s16AllocationMethod);
    APPL_TRACE_IMP("subbands: %u", btif_media_cb.encoder.s16NumOfSubBands);
    APPL_TRACE_IMP("num of blocks: %u", btif_media_cb.encoder.s16NumOfBlocks);
    APPL_TRACE_IMP("bitpool: <%u>,<%u>", btif_media_cb.min_bitpool, btif_media_cb.max_bitpool);
    APPL_TRACE_IMP("Scmst flag: %u", bta_av_co_cp_get_flag());

    param[0] = VS_QHCI_WRITE_SBC_CFG;
    param[1] = (UINT8)((1 << (3 - btif_media_cb.encoder.s16ChannelMode)) |
            (1 << (7 - btif_media_cb.encoder.s16SamplingFreq)));
    param[2] = (UINT8)((1 << btif_media_cb.encoder.s16AllocationMethod) |
            (1 << (3 - (btif_media_cb.encoder.s16NumOfSubBands >> 3))) |
            (1 << (7 - ((btif_media_cb.encoder.s16NumOfBlocks - 4) >> 2))));
    param[3] = btif_media_cb.min_bitpool;
    param[4] = btif_media_cb.max_bitpool;
    param[5] = 0; // Not in use as latency calculation will now be taken care of in SOC
    param[6] = 0; // Not in use as latency calculation will now be taken care of in SOC
    param[7] = 0; // Not in use as latency calculation will now be taken care of in SOC
    param[8] = 0; // Not in use as latency calculation will now be taken care of in SOC
    param[9] = 0; // 0 as delayed report not supported
#if (BTA_AV_CO_CP_SCMS_T == TRUE)
    param[10] = 1;
#else
    param[10] = 0;
#endif
    param[11] = bta_av_co_cp_get_flag();

    return BTA_DmVendorSpecificCommand(HCI_VSQC_CONTROLLER_A2DP_OPCODE, 12,
                                    param, btif_media_a2dp_write_sbc_cfg_cb);
}

void btif_media_pref_bit_rate_cb(tBTM_VSC_CMPL *param)
{
    unsigned char status = 0;
    BT_HDR *p_buf;

    if (param->param_len)
    {
        status = param->p_param_buf[0];
    }
    APPL_TRACE_IMP("VS_QHCI_A2DP_WRITE_SUGGESTED_BITRATE sent with error code: %u", status);

    if ((!status) && (NULL != (p_buf = GKI_getbuf(sizeof(BT_HDR)))))
    {
        p_buf->event = BTIF_MEDIA_VS_A2DP_PREF_BIT_RATE_SUCCESS;
        fixed_queue_enqueue(btif_media_cmd_msg_queue, p_buf);
    }
    else
    {
        APPL_TRACE_ERROR("Error in processing Vendor command response");
        a2dp_cmd_acknowledge(A2DP_CTRL_ACK_FAILURE);
    }
}

BOOLEAN btif_media_send_vendor_pref_bit_rate()
{
    UINT8 param[3];

    APPL_TRACE_IMP("btif_media_send_vendor_pref_bit_rate: bitrate: %d", btif_media_cb.encoder.u16BitRate);

    param[0] = VS_QHCI_A2DP_WRITE_SUGGESTED_BITRATE;
    param[1] = (UINT8)(btif_media_cb.encoder.u16BitRate & 0x00ff);
    param[2] = (UINT8)((btif_media_cb.encoder.u16BitRate & 0xff00) >> 8);

    return BTA_DmVendorSpecificCommand(HCI_VSQC_CONTROLLER_A2DP_OPCODE, 3,
                                            param, btif_media_pref_bit_rate_cb);
}

void btif_media_scmst_cb(tBTM_VSC_CMPL *param)
{
    unsigned char status = 0;
    BT_HDR *p_buf;

    if (param->param_len)
    {
        status = param->p_param_buf[0];
    }
    APPL_TRACE_IMP("VS_QHCI_A2DP_WRITE_SCMS_T_CP sent with error code: %u", status);

    if ((!status) && (NULL != (p_buf = GKI_getbuf(sizeof(BT_HDR)))))
    {
        p_buf->event = BTIF_MEDIA_VS_A2DP_SET_SCMST_HDR_SUCCESS;
        fixed_queue_enqueue(btif_media_cmd_msg_queue, p_buf);
    }
    else
    {
        APPL_TRACE_ERROR("Error in processing Vendor command response");
        a2dp_cmd_acknowledge(A2DP_CTRL_ACK_FAILURE);
    }
}

BOOLEAN btif_media_send_vendor_scmst_hdr()
{
    UINT8 param[3];

    APPL_TRACE_IMP("btif_media_send_vendor_pref_bit_rate");

    param[0] = VS_QHCI_A2DP_WRITE_SCMS_T_CP;
    param[1] = bta_av_co_cp_get_flag();

    return BTA_DmVendorSpecificCommand(HCI_VSQC_CONTROLLER_A2DP_OPCODE, 2,
                                            param, btif_media_pref_bit_rate_cb);
}

#endif

#endif /* BTA_AV_INCLUDED == TRUE */

/*******************************************************************************
 **
 ** Function         dump_codec_info
 **
 ** Description      Decode and display codec_info (for debug)
 **
 ** Returns          void
 **
 *******************************************************************************/
void dump_codec_info(unsigned char *p_codec)
{
    tA2D_STATUS a2d_status;
    tA2D_SBC_CIE sbc_cie;

    a2d_status = A2D_ParsSbcInfo(&sbc_cie, p_codec, FALSE);
    if (a2d_status != A2D_SUCCESS)
    {
        APPL_TRACE_ERROR("ERROR dump_codec_info A2D_ParsSbcInfo fail:%d", a2d_status);
        return;
    }

    APPL_TRACE_DEBUG("dump_codec_info");

    if (sbc_cie.samp_freq == A2D_SBC_IE_SAMP_FREQ_16)
    {    APPL_TRACE_DEBUG("\tsamp_freq:%d (16000)", sbc_cie.samp_freq);}
    else  if (sbc_cie.samp_freq == A2D_SBC_IE_SAMP_FREQ_32)
    {    APPL_TRACE_DEBUG("\tsamp_freq:%d (32000)", sbc_cie.samp_freq);}
    else  if (sbc_cie.samp_freq == A2D_SBC_IE_SAMP_FREQ_44)
    {    APPL_TRACE_DEBUG("\tsamp_freq:%d (44.100)", sbc_cie.samp_freq);}
    else  if (sbc_cie.samp_freq == A2D_SBC_IE_SAMP_FREQ_48)
    {    APPL_TRACE_DEBUG("\tsamp_freq:%d (48000)", sbc_cie.samp_freq);}
    else
    {    APPL_TRACE_DEBUG("\tBAD samp_freq:%d", sbc_cie.samp_freq);}

    if (sbc_cie.ch_mode == A2D_SBC_IE_CH_MD_MONO)
    {    APPL_TRACE_DEBUG("\tch_mode:%d (Mono)", sbc_cie.ch_mode);}
    else  if (sbc_cie.ch_mode == A2D_SBC_IE_CH_MD_DUAL)
    {    APPL_TRACE_DEBUG("\tch_mode:%d (Dual)", sbc_cie.ch_mode);}
    else  if (sbc_cie.ch_mode == A2D_SBC_IE_CH_MD_STEREO)
    {    APPL_TRACE_DEBUG("\tch_mode:%d (Stereo)", sbc_cie.ch_mode);}
    else  if (sbc_cie.ch_mode == A2D_SBC_IE_CH_MD_JOINT)
    {    APPL_TRACE_DEBUG("\tch_mode:%d (Joint)", sbc_cie.ch_mode);}
    else
    {    APPL_TRACE_DEBUG("\tBAD ch_mode:%d", sbc_cie.ch_mode);}

    if (sbc_cie.block_len == A2D_SBC_IE_BLOCKS_4)
    {    APPL_TRACE_DEBUG("\tblock_len:%d (4)", sbc_cie.block_len);}
    else  if (sbc_cie.block_len == A2D_SBC_IE_BLOCKS_8)
    {    APPL_TRACE_DEBUG("\tblock_len:%d (8)", sbc_cie.block_len);}
    else  if (sbc_cie.block_len == A2D_SBC_IE_BLOCKS_12)
    {    APPL_TRACE_DEBUG("\tblock_len:%d (12)", sbc_cie.block_len);}
    else  if (sbc_cie.block_len == A2D_SBC_IE_BLOCKS_16)
    {    APPL_TRACE_DEBUG("\tblock_len:%d (16)", sbc_cie.block_len);}
    else
    {    APPL_TRACE_DEBUG("\tBAD block_len:%d", sbc_cie.block_len);}

    if (sbc_cie.num_subbands == A2D_SBC_IE_SUBBAND_4)
    {    APPL_TRACE_DEBUG("\tnum_subbands:%d (4)", sbc_cie.num_subbands);}
    else  if (sbc_cie.num_subbands == A2D_SBC_IE_SUBBAND_8)
    {    APPL_TRACE_DEBUG("\tnum_subbands:%d (8)", sbc_cie.num_subbands);}
    else
    {    APPL_TRACE_DEBUG("\tBAD num_subbands:%d", sbc_cie.num_subbands);}

    if (sbc_cie.alloc_mthd == A2D_SBC_IE_ALLOC_MD_S)
    {    APPL_TRACE_DEBUG("\talloc_mthd:%d (SNR)", sbc_cie.alloc_mthd);}
    else  if (sbc_cie.alloc_mthd == A2D_SBC_IE_ALLOC_MD_L)
    {    APPL_TRACE_DEBUG("\talloc_mthd:%d (Loundess)", sbc_cie.alloc_mthd);}
    else
    {    APPL_TRACE_DEBUG("\tBAD alloc_mthd:%d", sbc_cie.alloc_mthd);}

    APPL_TRACE_DEBUG("\tBit pool Min:%d Max:%d", sbc_cie.min_bitpool, sbc_cie.max_bitpool);

}

