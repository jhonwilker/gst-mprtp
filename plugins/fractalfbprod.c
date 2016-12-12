/* GStreamer Scheduling tree
 * Copyright (C) 2015 Balázs Kreith (contact: balazs.kreith@gmail.com)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/rtp/gstrtpbuffer.h>
#include <gst/rtp/gstrtcpbuffer.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>     /* qsort */
#include "fractalfbprod.h"
#include "reportprod.h"

#define _now(this) gst_clock_get_time (this->sysclock)

GST_DEBUG_CATEGORY_STATIC (fractalfbproducer_debug_category);
#define GST_CAT_DEFAULT fractalfbproducer_debug_category

G_DEFINE_TYPE (FRACTaLFBProducer, fractalfbproducer, G_TYPE_OBJECT);

typedef struct{
  guint64 qdelay_est;
//  gint32  stability;
  guint   ref;
}QDelayEst;
DEFINE_RECYCLE_TYPE(static, qdelay, QDelayEst)

static void _qdelay_item_shape(QDelayEst* result, gpointer udata)
{
  memset(result, 0, sizeof(QDelayEst));
}

static void _on_qdelay_item_ref(FRACTaLFBProducer* this, QDelayEst* item){
  ++item->ref;
}

static void _on_qdelay_item_unref(FRACTaLFBProducer* this, QDelayEst* item){
  if(0 < --item->ref){
    return;
  }
  recycle_add(this->qdelay_recycle, item);
}

static void fractalfbproducer_finalize (GObject * object);
static gboolean _do_fb(FRACTaLFBProducer* data);;
static gboolean _packet_subflow_filter(FRACTaLFBProducer *this, RcvPacket *packet);
static void _on_discarded_packet(FRACTaLFBProducer *this, RcvPacket *packet);
static void _on_received_packet(FRACTaLFBProducer *this, RcvPacket *packet);
static void _setup_xr_rfc7243(FRACTaLFBProducer * this,ReportProducer* reportproducer);
static void _setup_xr_rfc3611_rle_lost(FRACTaLFBProducer * this,  ReportProducer* reportproducer);
static void _setup_xr_owd(FRACTaLFBProducer * this,  ReportProducer* reportproducer);
//static void _setup_afb_reps(FRACTALFBProducer * this, ReportProducer *reportproducer);
static void _on_fb_update(FRACTaLFBProducer *this,  ReportProducer* reportproducer);

static gint
_cmp_seq (guint16 x, guint16 y)
{
  if(x == y) return 0;
  if(x < y && y - x < 32768) return -1;
  if(x > y && x - y > 32768) return -1;
  if(x < y && y - x > 32768) return 1;
  if(x > y && x - y < 32768) return 1;
  return 0;
}

static gint
_cmp_ts (guint32 x, guint32 y)
{
  if(x == y) return 0;
  if(x < y && y - x < 2147483648) return -1;
  if(x > y && x - y > 2147483648) return -1;
  if(x < y && y - x > 2147483648) return 1;
  if(x > y && x - y < 2147483648) return 1;
  return 0;
}


static guint16 _diff_seq(guint16 a, guint16 b)
{
  if(a < b) return b-a;
  if(b < a) return (65536 - a) + b;
  return 0;
}

static void _on_rle_sw_rem(FRACTaLFBProducer* this, guint16* seq_num)
{
  this->vector[*seq_num] = FALSE;
  if(_cmp_seq(this->begin_seq, *seq_num) <= 0){
    this->begin_seq = *seq_num + 1;
  }
}


PercentileResultPipeFnc(_owd_percentile_pipe, FRACTaLFBProducer, median_delay, min_delay, max_delay, QDelayEst, qdelay_est, 0);
//PercentileRawResultPipeFnc(_owd_percentile_pipe, FRACTaLFBProducer, GstClockTime, median_delay, min_delay, max_delay, 0);

void
fractalfbproducer_class_init (FRACTaLFBProducerClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = (GObjectClass *) klass;

  gobject_class->finalize = fractalfbproducer_finalize;

  GST_DEBUG_CATEGORY_INIT (fractalfbproducer_debug_category, "fractalfbproducer", 0,
      "FRACTALFBProducer");

}

void
fractalfbproducer_finalize (GObject * object)
{
  FRACTaLFBProducer *this;
  this = FRACTALFBPRODUCER(object);

  rcvtracker_rem_on_received_packet_listener(this->tracker,  (ListenerFunc)_on_received_packet);
  rcvsubflow_rem_on_rtcp_fb_cb(this->subflow, (ListenerFunc) _on_fb_update);

  g_object_unref(this->sysclock);
  g_object_unref(this->tracker);
  mprtp_free(this->vector);


}

void
fractalfbproducer_init (FRACTaLFBProducer * this)
{
  this->sysclock = gst_system_clock_obtain();

  this->vector   = g_malloc0(sizeof(gboolean)  * 65536);
  this->vector_length = 0;

}

//static void _owd_sprint(gpointer item, gchar *result)
//{
//  sprintf(result, "%lu", GST_TIME_AS_MSECONDS(*(guint64*)item));
//}

FRACTaLFBProducer *make_fractalfbproducer(RcvSubflow* subflow, RcvTracker *tracker)
{
  FRACTaLFBProducer *this;
  this = g_object_new (FRACTALFBPRODUCER_TYPE, NULL);
  this->subflow         = subflow;
  this->tracker         = g_object_ref(tracker);

  this->qdelay_recycle  = make_recycle_qdelay(256, NULL);
  this->owds_sw         = make_slidingwindow_uint64(20, 500 * GST_MSECOND);

  this->rle_sw          = make_slidingwindow_uint16(100, GST_SECOND);

  slidingwindow_add_on_data_ref_change(this->owds_sw,
      (ListenerFunc) _on_qdelay_item_ref, (ListenerFunc) _on_qdelay_item_unref, this);

  slidingwindow_add_on_rem_item_cb(this->rle_sw, (ListenerFunc) _on_rle_sw_rem, this);

//  slidingwindow_add_plugin(this->owds_sw, make_swprinter(_owd_sprint));
  slidingwindow_add_plugin(this->owds_sw,
      make_swpercentile(50, bintree3cmp_uint64, (ListenerFunc)_owd_percentile_pipe, this));

  rcvtracker_add_on_received_packet_listener_with_filter(this->tracker,
      (ListenerFunc) _on_received_packet,
      (ListenerFilterFunc) _packet_subflow_filter,
      this);

  rcvtracker_add_on_discarded_packet_listener_with_filter(this->tracker,
      (ListenerFunc) _on_discarded_packet,
      (ListenerFilterFunc) _packet_subflow_filter,
      this);

  rcvsubflow_add_on_rtcp_fb_cb(subflow, (ListenerFunc) _on_fb_update, this);

  return this;
}

void fractalfbproducer_reset(FRACTaLFBProducer *this)
{
  this->initialized = FALSE;
}

void fractalfbproducer_set_owd_treshold(FRACTaLFBProducer *this, GstClockTime treshold)
{
  slidingwindow_set_treshold(this->owds_sw, treshold);
}

gboolean _packet_subflow_filter(FRACTaLFBProducer *this, RcvPacket *packet)
{
  return packet->subflow_id == this->subflow->id;
}

void _on_discarded_packet(FRACTaLFBProducer *this, RcvPacket *packet)
{
  this->discarded_bytes += packet->payload_size;
}

static void _refresh_frame_delay(FRACTaLFBProducer *this, RcvPacket* packet)
{
  gint32 cmp;
  if(this->prev_ts == 0){
    this->prev_ts       = packet->timestamp;
    this->prev_rcv      = 0;
    this->head_snd      = this->prev_snd = packet->abs_snd_ntp_chunk;
    this->head_rcv      = packet->abs_rcv_ntp_time;
    this->sending_delay = 0;
    this->frame_delay   = 0;
    this->prev_qdelay   = 0;
    return;
  }
  if(0 < _cmp_seq(this->prev_seq, packet->abs_seq)){
    return;
  }
  cmp = _cmp_ts(this->prev_ts, packet->timestamp);
  if(0 <= cmp){
    if(cmp == 0){//refresh the sending/pacing delay in ntp units
      this->sending_delay += packet->abs_snd_ntp_chunk - this->prev_snd;
      this->prev_snd       = packet->abs_snd_ntp_chunk;
    }
    return;
  }

  if(packet->abs_snd_ntp_chunk < this->head_snd){//turnaround
    g_print("TURNAROUND? %lu->%lX %lu->%lX\n",
        packet->abs_snd_ntp_chunk, packet->abs_snd_ntp_chunk,
        this->head_snd, this->head_snd);
    this->frame_delay = 0x0000004000000000UL - this->head_snd + packet->abs_snd_ntp_chunk;
  }else{
    this->frame_delay = packet->abs_snd_ntp_chunk - this->head_snd;
  }

  this->prev_rcv      = this->head_rcv;
  this->head_rcv      = packet->abs_rcv_ntp_time;
  this->sending_delay = 0;
  this->head_snd      = this->prev_snd = packet->abs_snd_ntp_chunk;
  this->prev_ts       = packet->timestamp;
}

static void _add_new_queue_delay_est(FRACTaLFBProducer *this, RcvPacket* packet)
{
  QDelayEst *item;
  guint64 est_rcv_ntp_time;

  _refresh_frame_delay(this, packet);
  if(this->frame_delay == 0){
    return;
  }

  est_rcv_ntp_time = this->prev_rcv + this->frame_delay + this->sending_delay;
  item = recycle_retrieve_and_shape(this->qdelay_recycle, _qdelay_item_shape);
  if(packet->abs_rcv_ntp_time < est_rcv_ntp_time){
    if(packet->abs_rcv_ntp_time < est_rcv_ntp_time - this->prev_qdelay){
      item->qdelay_est = 0;
    }else{
      item->qdelay_est = this->prev_qdelay - (est_rcv_ntp_time - packet->abs_rcv_ntp_time);
    }
  }else{
    item->qdelay_est = packet->abs_rcv_ntp_time - est_rcv_ntp_time;
  }
  this->prev_qdelay = item->qdelay_est;
//  g_print("Added qdelay est: %lu, median: %lu\n",
//      get_epoch_time_from_ntp_in_ns(item->qdelay_est),
//      get_epoch_time_from_ntp_in_ns(this->median_delay));
  slidingwindow_add_data(this->owds_sw, item);
}

void _on_received_packet(FRACTaLFBProducer *this, RcvPacket *packet)
{
  _add_new_queue_delay_est(this, packet);
//  slidingwindow_add_data(this->owds_sw, &packet->delay);
  slidingwindow_add_data(this->rle_sw,  &packet->subflow_seq);

  ++this->rcved_packets;
  this->vector[packet->subflow_seq] = TRUE;
  if(!this->initialized){
    this->initialized = TRUE;
    this->begin_seq = this->end_seq = packet->subflow_seq;
    goto done;
  }

  if(_cmp_seq(this->end_seq, packet->subflow_seq) < 0){
    this->end_seq = packet->subflow_seq;
  }

done:
  return;
}


static gboolean _do_fb(FRACTaLFBProducer *this)
{
  GstClockTime now = _now(this);
  if(now - 20 * GST_MSECOND < this->last_fb){
    return FALSE;
  }
  if(this->last_fb < _now(this) - 100 * GST_MSECOND){
    return TRUE;
  }
  return 3 < this->rcved_packets;
}


void _on_fb_update(FRACTaLFBProducer *this, ReportProducer* reportproducer)
{
  if(!_do_fb(this)){
    goto done;
  }

  report_producer_begin(reportproducer, this->subflow->id);
  //Okay, so discarded byte metrics indicate incipient congestion,
  //which is in fact indicated by the owd distoration either. This is one point of the discard this metric :)
  //another point is the fact that if competing with tcp, tcp pushes the netqueue
  //until its limitation, thus discard metrics always appear, and also
  //if jitter is high discard metrics appear naturally
  //so now on we try to not to rely on this metric, but for owd and losts.
  DISABLE_LINE _setup_xr_rfc7243(this, reportproducer);
  _setup_xr_owd(this, reportproducer);
  _setup_xr_rfc3611_rle_lost(this, reportproducer);

  this->last_fb = _now(this);
  this->rcved_packets = 0;
done:
  return;
}

void _setup_xr_rfc7243(FRACTaLFBProducer * this,ReportProducer* reportproducer)
{
  gboolean interval_metric_flag = TRUE;
  gboolean early_bit = FALSE;

  report_producer_add_xr_discarded_bytes(reportproducer,
                                         interval_metric_flag,
                                         early_bit,
                                         this->discarded_bytes
                                        );
  this->discarded_bytes = 0;
}

void _setup_xr_rfc3611_rle_lost(FRACTaLFBProducer * this,ReportProducer* reportproducer)
{

  if(_cmp_seq(this->end_seq, this->begin_seq) <= 0){
    goto done;
  }

  this->vector_length = _diff_seq(this->begin_seq, this->end_seq) + 1;

  report_producer_add_xr_lost_rle(reportproducer,
                                       FALSE,
                                       0,
                                       this->begin_seq,
                                       this->end_seq,
                                       this->vector + this->begin_seq,
                                       this->vector_length
                                       );

//  g_print("FB creating begin seq: %d end seq: %d, vector length: %d\n", this->begin_seq, this->end_seq, this->vector_length);
  //BAD!
//  memset(this->vector, 0, sizeof(gboolean) * 65536);
//  if(_cmp_seq(this->begin_seq, this->end_seq) < 0){
//    this->begin_seq = this->end_seq + 1;
//  }
done:
  return;
}



//void _setup_xr_owd(FRACTaLFBProducer * this, ReportProducer* reportproducer)
//{
//  guint32      u32_median_delay, u32_min_delay, u32_max_delay;
//
//  u32_median_delay = (guint32)(get_ntp_from_epoch_ns(this->median_delay)>>16);
//  u32_min_delay    = (guint32)(get_ntp_from_epoch_ns(this->min_delay)>>16);
//  u32_max_delay    = (guint32)(get_ntp_from_epoch_ns(this->max_delay)>>16);
//
//  report_producer_add_xr_owd(reportproducer,
//                             RTCP_XR_RFC7243_I_FLAG_CUMULATIVE_DURATION,
//                             u32_median_delay,
//                             u32_min_delay,
//                             u32_max_delay);
//
//
//}

void _setup_xr_owd(FRACTaLFBProducer * this, ReportProducer* reportproducer)
{
  guint32      u32_median_delay, u32_min_delay, u32_max_delay;

  u32_median_delay = this->median_delay >> 16;
  u32_min_delay    = this->min_delay >> 16;
  u32_max_delay    = this->max_delay >> 16;

  report_producer_add_xr_owd(reportproducer,
                             RTCP_XR_RFC7243_I_FLAG_CUMULATIVE_DURATION,
                             u32_median_delay,
                             u32_min_delay,
                             u32_max_delay);


}



