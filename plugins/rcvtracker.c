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
#include <gst/gst.h>
#include <stdlib.h>
#include <string.h>
#include "rcvctrler.h"
#include "rcvpackets.h"


GST_DEBUG_CATEGORY_STATIC (rcvtracker_debug_category);
#define GST_CAT_DEFAULT rcvtracker_debug_category

#define THIS_READLOCK(this) g_rw_lock_reader_lock(&this->rwmutex)
#define THIS_READUNLOCK(this) g_rw_lock_reader_unlock(&this->rwmutex)
#define THIS_WRITELOCK(this) g_rw_lock_writer_lock(&this->rwmutex)
#define THIS_WRITEUNLOCK(this) g_rw_lock_writer_unlock(&this->rwmutex)

#define _now(this) gst_clock_get_time (this->sysclock)
#define _priv(this) ((Private*)(this->priv))
//#define _get_subflow(this, subflow_id) ((Subflow*)(_priv(this)->subflows + subflow_id))

G_DEFINE_TYPE (RcvTracker, rcvtracker, G_TYPE_OBJECT);

typedef struct _Subflow{
  gboolean              initialized;
  RcvTrackerSubflowStat stat;

  guint32               last_rcv_rtp_ts;
  guint32               last_snd_rtp_ts;
  gboolean              seq_initialized;
}Subflow;

typedef struct _Priv{
  Subflow subflows[256];
}Private;

typedef struct{
  GstClockTime timestamp;
}GstClockTimeCover;

static void _init_subflow(RcvTracker *this, Subflow* subflow);
static Subflow* _get_subflow(RcvTracker *this, guint8 subflow_id);

static Private* _priv_ctor(void);
static void _priv_dtor(Private *priv);
//----------------------------------------------------------------------
//-------- Private functions belongs to the object ----------
//----------------------------------------------------------------------

static void
rcvtracker_finalize (
    GObject * object);


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

static guint32 _delta_ts(guint32 last_ts, guint32 actual_ts) {
  if (last_ts <= actual_ts) {
    return actual_ts - last_ts;
  } else {
    return 4294967296 - last_ts + actual_ts;
  }
}

//----------------------------------------------------------------------
//--------- Private functions implementations to SchTree object --------
//----------------------------------------------------------------------

void
rcvtracker_class_init (RcvTrackerClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = (GObjectClass *) klass;

  gobject_class->finalize = rcvtracker_finalize;

  GST_DEBUG_CATEGORY_INIT (rcvtracker_debug_category, "rcvtracker", 0,
      "MpRTP Sending Rate Distributor");
}

RcvTracker *make_rcvtracker(void)
{
  RcvTracker* this;
  this = g_object_new(RCVTRACKER_TYPE, NULL);
  this->cc_ts_generator = make_timestamp_generator(DEFAULT_CC_TIMESTAMP_GENERATOR_CLOCKRATE);
  this->rtp_ts_generator = make_timestamp_generator(DEFAULT_RTP_TIMESTAMP_GENERATOR_CLOCKRATE);

  return this;
}

TimestampGenerator* rcvtracker_get_cc_ts_generator(RcvTracker* this) {
  return this->cc_ts_generator;
}

TimestampGenerator* rcvtracker_get_rtp_ts_generator(RcvTracker* this) {
  return this->rtp_ts_generator;
}

void
rcvtracker_finalize (GObject * object)
{
  RcvTracker * this;
  this = RCVTRACKER(object);
  g_object_unref(this->sysclock);
  g_object_unref(this->on_discarded_packet);
  g_object_unref(this->on_received_packet);
  g_object_unref(this->on_lost_packet);
  g_object_unref(this->cc_ts_generator);
  g_object_unref(this->rtp_ts_generator);
  _priv_dtor(this->priv);
}


void
rcvtracker_init (RcvTracker * this)
{
  this->sysclock = gst_system_clock_obtain();
  this->priv                 = _priv_ctor();
  this->on_discarded_packet  = make_notifier("RcvTracker: on-discarded-packet");
  this->on_received_packet   = make_notifier("RcvTracker: on-received-packet");
  this->on_lost_packet       = make_notifier("RcvTracker: on-lost-packet");

}


static void _rcvtracker_refresh_helper(Subflow* subflow, RcvTracker *this)
{

}

void rcvtracker_refresh(RcvTracker * this)
{
  g_slist_foreach(this->joined_subflows, (GFunc) _rcvtracker_refresh_helper, this);
}

void rcvtracker_on_recovered_buffer(RcvTracker* this, GstBuffer* repairedbuf)
{
  ++this->stat.recovered_packets;
}

void rcvtracker_add_discarded_packet(RcvTracker* this, RcvPacket* packet)
{
  ++this->stat.discarded_packets;
  notifier_do(this->on_discarded_packet, packet);
}

void rcvtracker_add_on_received_packet_listener(RcvTracker * this, ListenerFunc callback, gpointer udata)
{
  notifier_add_listener(this->on_received_packet, callback, udata);
}

void rcvtracker_add_on_received_packet_listener_with_filter(RcvTracker * this, ListenerFunc callback, ListenerFilterFunc filter, gpointer udata)
{
  notifier_add_listener_with_filter(this->on_received_packet, callback, filter, udata);
}

void rcvtracker_rem_on_received_packet_listener(RcvTracker * this, ListenerFunc callback)
{
  notifier_rem_listener(this->on_received_packet, callback);
}


void rcvtracker_add_on_discarded_packet_listener(RcvTracker * this,
                                    ListenerFunc callback,
                                    gpointer udata)
{
  notifier_add_listener(this->on_discarded_packet, callback, udata);
}

void rcvtracker_add_on_discarded_packet_listener_with_filter(RcvTracker * this,
                                    ListenerFunc callback,
                                    ListenerFilterFunc filter,
                                    gpointer udata)
{
  notifier_add_listener_with_filter(this->on_discarded_packet, callback, filter, udata);
}


RcvTrackerStat* rcvtracker_get_stat(RcvTracker * this)
{
  return &this->stat;
}

RcvTrackerSubflowStat* rcvtracker_get_subflow_stat(RcvTracker * this, guint8 subflow_id)
{
  return &_get_subflow(this, subflow_id)->stat;
}

static void _subflow_add_packet(RcvTracker * this, Subflow *subflow, RcvPacket* packet)
{
  gint cmp;

  if (subflow->seq_initialized == FALSE) {
    subflow->stat.highest_seq = packet->subflow_seq;
    subflow->stat.total_received_packets = 1;
    subflow->stat.total_received_bytes = packet->payload_size;
    subflow->last_rcv_rtp_ts = packet->rcv_rtp_ts;
    subflow->last_snd_rtp_ts = packet->snd_rtp_ts;
    subflow->seq_initialized = TRUE;
    goto done;
  }

  packet->subflow_skew_in_ts = (gint64)_delta_ts(subflow->last_rcv_rtp_ts, packet->rcv_rtp_ts) - (gint64)_delta_ts(subflow->last_snd_rtp_ts, packet->snd_rtp_ts);
  packet->subflow_skew_in_ts = (packet->subflow_skew_in_ts < 0 ? -1 * packet->subflow_skew_in_ts : packet->subflow_skew_in_ts);
  subflow->stat.jitter += abs(packet->subflow_skew_in_ts - subflow->stat.jitter) / 32;
  packet->subflow_jitter_at_rcv = subflow->stat.jitter;

  subflow->last_rcv_rtp_ts = packet->rcv_rtp_ts; //TODO: this is a chaos if the generator clock rate not match to the rtp packet clock rate
  subflow->last_snd_rtp_ts = packet->snd_rtp_ts;
  subflow->stat.cycle_num = (++subflow->stat.total_received_packets)>>16;
  subflow->stat.total_received_bytes += packet->payload_size;

  cmp = _cmp_seq(packet->subflow_seq, subflow->stat.highest_seq + 1);
  if(cmp < 0){
    goto done;
  }

  if(!cmp){
    subflow->stat.highest_seq = packet->subflow_seq;
  }else{
    guint16 act_seq;
    for(act_seq = subflow->stat.highest_seq; act_seq != packet->subflow_seq; ++act_seq){
      LostPacket lost_packet;
      lost_packet.subflow_seq = act_seq;
      lost_packet.subflow_id  = packet->subflow_id;
      notifier_do(this->on_lost_packet, &lost_packet);
    }
    subflow->stat.highest_seq = packet->subflow_seq;
  }

done:
  return;
}

void rcvtracker_add_packet(RcvTracker * this, RcvPacket* packet)
{
  packet->rcv_rtp_ts = timestamp_generator_get_ts(this->rtp_ts_generator);

  if(packet->subflow_id != 0){
    Subflow* subflow = _get_subflow(this, packet->subflow_id);
    _subflow_add_packet(this, subflow, packet);
  }
  notifier_do(this->on_received_packet, packet);
}


Subflow* _get_subflow(RcvTracker *this, guint8 subflow_id)
{
  Subflow *result;
  result = _priv(this)->subflows + subflow_id;
  if(!result->initialized){
    _init_subflow(this, result);
    result->initialized = TRUE;
  }
  return result;
}




void _init_subflow(RcvTracker *this, Subflow* subflow)
{
  this->joined_subflows = g_slist_prepend(this->joined_subflows, subflow);
}


static Private* _priv_ctor(void)
{
  Private* result = g_malloc0(sizeof(Private));
  return result;
}

static void _priv_dtor(Private *priv)
{

}
