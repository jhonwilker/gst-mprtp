
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/rtp/gstrtpbuffer.h>
#include <gst/rtp/gstrtcpbuffer.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "mprtprpath.h"
#include "mprtpspath.h"
#include "gstmprtcpbuffer.h"
#include "streamjoiner.h"

GST_DEBUG_CATEGORY_STATIC (gst_mprtpr_path_debug_category);
#define GST_CAT_DEFAULT gst_mprtpr_path_debug_category

G_DEFINE_TYPE (MpRTPRPath, mprtpr_path, G_TYPE_OBJECT);


#define THIS_READLOCK(this) g_rw_lock_reader_lock(&this->rwmutex)
#define THIS_READUNLOCK(this) g_rw_lock_reader_unlock(&this->rwmutex)
#define THIS_WRITELOCK(this) g_rw_lock_writer_lock(&this->rwmutex)
#define THIS_WRITEUNLOCK(this) g_rw_lock_writer_unlock(&this->rwmutex)

#define _actual_RLEBlock(this) ((RLEBlock*)(this->rle.blocks + this->rle.write_index))
#define _now(this) (gst_clock_get_time(this->sysclock))

#define _discrle(this) this->discard_rle
#define _actual_discrle(this) ((DiscardRLEBlock*)(this->discard_rle.blocks + this->discard_rle.write_index))
#define _lostrle(this) this->losts_rle
#define _actual_lostrle(this) ((LostsRLEBlock*)(this->losts_rle.blocks + this->losts_rle.write_index))
#define _owdrle(this) this->owd_rle
#define _actual_owdrle(this) ((OWDRLEBlock*)(this->owd_rle.blocks + this->owd_rle.write_index))
#define _skewrle(this) this->skew_rle
#define _actual_skewrle(this) ((SkewRLEBlock*)(this->skew_rle.blocks + this->skew_rle.write_index))


static void mprtpr_path_finalize (GObject * object);
static void mprtpr_path_reset (MpRTPRPath * this);
static gint _cmp_seq (guint16 x, guint16 y);
static void _add_discard(MpRTPRPath *this, GstMpRTPBuffer *mprtp);
static void _add_delay(MpRTPRPath *this, GstClockTime delay);
static void _add_skew(MpRTPRPath *this, gint64 skew);
static void _refresh_RLEBlock(MpRTPRPath *this);
static void _refresh_RLE(MpRTPRPath *this);
static void _delays_stats_pipe(gpointer data, PercentileTrackerPipeData *pdata);
static void _gaps_obsolation_pipe(gpointer data, gint64 seq);

void
mprtpr_path_class_init (MpRTPRPathClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = (GObjectClass *) klass;

  gobject_class->finalize = mprtpr_path_finalize;

  GST_DEBUG_CATEGORY_INIT (gst_mprtpr_path_debug_category, "mprtpr_path",
      0, "MPRTP Receiver Subflow");
}


MpRTPRPath *
make_mprtpr_path (guint8 id)
{
  MpRTPRPath *result;

  result = g_object_new (MPRTPR_PATH_TYPE, NULL);
  THIS_WRITELOCK (result);
  result->id = id;
  THIS_WRITEUNLOCK (result);
  return result;
}


void
mprtpr_path_init (MpRTPRPath * this)
{
  g_rw_lock_init (&this->rwmutex);
  this->sysclock = gst_system_clock_obtain ();
  this->delays = make_percentiletracker(512, 50);
  percentiletracker_set_treshold(this->delays, 1000 * GST_MSECOND);
  percentiletracker_set_stats_pipe(this->delays, _delays_stats_pipe, this);

  this->skews = make_percentiletracker2(100, 50);
  percentiletracker2_set_treshold(this->skews, 1 * GST_SECOND);

  _owdrle(this).last_step = _now(this);
  _owdrle(this).read_index = _owdrle(this).write_index = 0;
  _owdrle(this).step_interval = 125 * GST_MSECOND;

  _lostrle(this).last_step = _now(this);
  _lostrle(this).read_index = _lostrle(this).write_index = 0;
  _lostrle(this).step_interval = GST_SECOND;

  _discrle(this).last_step = _now(this);
  _discrle(this).read_index = _discrle(this).write_index = 0;
  _discrle(this).step_interval = GST_SECOND;

  this->gaps = make_numstracker(1024, 1000 * GST_MSECOND);
  numstracker_add_rem_pipe(this->gaps, _gaps_obsolation_pipe, this);
  this->lates = make_numstracker(1024, 1500 * GST_MSECOND);
  mprtpr_path_reset (this);
}

void mprtpr_path_destroy(gpointer ptr)
{
  G_OBJECT_CLASS(mprtpr_path_parent_class)->finalize(G_OBJECT(ptr));
}

void
mprtpr_path_finalize (GObject * object)
{
  MpRTPRPath *this;
  this = MPRTPR_PATH_CAST (object);
  g_object_unref (this->sysclock);
//  g_object_unref(this->lt_low_delays);
//  g_object_unref(this->lt_high_delays);
//  g_object_unref(this->skews);
}


void
mprtpr_path_reset (MpRTPRPath * this)
{
  this->seq_initialized = FALSE;
  //this->skew_initialized = FALSE;
  this->cycle_num = 0;
  this->total_late_discarded = 0;
  this->total_late_discarded_bytes = 0;
  this->highest_seq = 0;
  this->jitter = 0;
  this->total_packets_received = 0;
  this->total_payload_bytes = 0;
  this->total_packet_losts = 0;
  this->last_packet_skew = 0;
  this->last_received_time = 0;

}

guint8
mprtpr_path_get_id (MpRTPRPath * this)
{
  guint8 result;
  THIS_READLOCK (this);
  result = this->id;
  THIS_READUNLOCK (this);
  return result;
}

void mprtpr_path_get_RR_stats(MpRTPRPath *this,
                              guint16 *HSN,
                              guint16 *cycle_num,
                              guint32 *jitter,
                              guint32 *received_num,
                              guint32 *total_lost,
                              guint32 *received_bytes)
{
  THIS_READLOCK (this);
  if(HSN) *HSN = this->highest_seq;
  if(cycle_num) *cycle_num = this->cycle_num;
  if(jitter) *jitter = this->jitter;
  if(total_lost) *total_lost = this->total_packet_losts;
  if(received_num) *received_num = this->total_packets_received;
  if(received_bytes) *received_bytes = this->total_payload_bytes;
  THIS_READUNLOCK (this);
}

void mprtpr_path_set_reported_sequence(MpRTPRPath *this, guint16 sequence_number)
{
  THIS_WRITELOCK (this);
  this->reported_sequence_number = sequence_number;
  THIS_WRITEUNLOCK (this);
}

void mprtpr_path_get_total_discards (MpRTPRPath * this,
                               guint16 *discarded,
                               guint32 *discarded_bytes)
{
  THIS_READLOCK (this);
  if(discarded) *discarded = this->total_late_discarded;
  if(discarded_bytes) *discarded_bytes = this->total_late_discarded_bytes;
  THIS_READUNLOCK (this);
}

void mprtpr_path_get_XR7243_stats(MpRTPRPath *this,
                           guint16 *discarded,
                           guint32 *discarded_bytes)
{
  THIS_READLOCK (this);
  if(discarded) *discarded = this->total_late_discarded - this->interval_late_discarded;
  if(discarded_bytes) *discarded_bytes = this->total_late_discarded_bytes - this->interval_late_discarded_bytes;
  this->interval_late_discarded = this->total_late_discarded;
  this->interval_late_discarded_bytes = this->total_late_discarded_bytes;
  THIS_READUNLOCK (this);
}


void mprtpr_path_get_XROWD_stats(MpRTPRPath *this,
                                 GstClockTime *median,
                                 GstClockTime *min,
                                 GstClockTime* max)
{
  GstClockTime median_delay;
  THIS_READLOCK (this);
  median_delay = percentiletracker_get_stats(this->delays, min, max, NULL);
  if(median) *median = median_delay;
  THIS_READUNLOCK (this);
}



gboolean mprtpr_path_request_urgent_report(MpRTPRPath *this)
{
  gboolean result;
  THIS_WRITELOCK (this);
  result = this->request_urgent_report;
  this->request_urgent_report = FALSE;
  THIS_WRITEUNLOCK (this);
  return result;
}

void
mprtpr_path_set_chunks_reported(MpRTPRPath *this)
{
  THIS_WRITELOCK (this);
  this->discard_rle.read_index = this->discard_rle.write_index;
  this->losts_rle.read_index = this->losts_rle.write_index;
  this->owd_rle.read_index = this->owd_rle.write_index;
  THIS_WRITEUNLOCK (this);
}

void
mprtpr_path_set_discard_latency(MpRTPRPath *this, GstClockTime latency)
{
  THIS_WRITELOCK (this);
  this->discard_latency = latency;
  THIS_WRITEUNLOCK (this);
}

void
mprtpr_path_set_lost_latency(MpRTPRPath *this, GstClockTime latency)
{
  THIS_WRITELOCK (this);
  numstracker_set_treshold(this->gaps,  latency);
  numstracker_set_treshold(this->lates, latency * 1.5);
  THIS_WRITEUNLOCK (this);
}


GstRTCPXR_Chunk *
mprtpr_path_get_discard_chunks(MpRTPRPath *this,
                              guint *chunks_num,
                              guint16 *begin_seq,
                              guint16 *end_seq)
{
  GstRTCPXR_Chunk *result = NULL, *chunk;
  gboolean chunk_type = FALSE;
  gboolean run_type;
  DiscardRLEBlock *block;
  guint16 begin_seq_, end_seq_;
  gint i,chunks_num_;
  guint16 *read;

  chunks_num_ = 0;
  THIS_READLOCK (this);
  for(i=this->discard_rle.read_index; ;){
    ++chunks_num_;
    if(i == this->discard_rle.write_index) break;
    if(++i == MPRTP_PLUGIN_MAX_RLE_LENGTH) i=0;
  }
  if((chunks_num_ % 2) == 1) ++chunks_num_;
  chunk = result = mprtp_malloc(sizeof(GstRTCPXR_Chunk) * chunks_num_);
  block = &this->discard_rle.blocks[this->discard_rle.read_index];
  begin_seq_ = block->start_seq;
  for(i=this->discard_rle.read_index; ; )
  {
    end_seq_ = block->end_seq;
    run_type = i != this->discard_rle.write_index;

    read = &this->discard_rle.blocks[i].discarded_bytes;

    gst_rtcp_xr_chunk_change(chunk,
                             &chunk_type,
                             &run_type,
                             read);
    if(i == this->discard_rle.write_index) break;
    ++chunk;
    ++block;
    if(++i == MPRTP_PLUGIN_MAX_RLE_LENGTH) i=0;
  }
  THIS_READUNLOCK (this);
  if(chunks_num) *chunks_num = chunks_num_;
  if(begin_seq) *begin_seq = begin_seq_;
  if(end_seq) *end_seq = end_seq_;
  return result;
}



GstRTCPXR_Chunk *
mprtpr_path_get_owd_chunks(MpRTPRPath *this,
                              guint *chunks_num,
                              guint16 *begin_seq,
                              guint16 *end_seq,
                              guint32 *offset)
{
  GstRTCPXR_Chunk *result = NULL, *chunk;
  gboolean chunk_type = FALSE;
  gboolean run_type;
  OWDRLEBlock *block;
  guint16 begin_seq_, end_seq_;
  gint i,chunks_num_;
  guint16 *read,running_length;
  GstClockTime abs_offset = -1;

  chunks_num_ = 0;
  THIS_READLOCK (this);
  for(i=this->owd_rle.read_index; ;){
    ++chunks_num_;
    if(i == this->owd_rle.write_index) break;
    if(++i == MPRTP_PLUGIN_MAX_RLE_LENGTH) i=0;
  }
  if((chunks_num_ % 2) == 1) ++chunks_num_;
  chunk = result = mprtp_malloc(sizeof(GstRTCPXR_Chunk) * chunks_num_);
  block = &this->owd_rle.blocks[this->owd_rle.read_index];
  begin_seq_ = block->start_seq;
  //get the offset
  for(i=this->owd_rle.read_index; ; )
  {
    {
      GstClockTime owd;
      owd = this->owd_rle.blocks[i].median_delay;
      if(owd < abs_offset){
        abs_offset = owd;
      }
    }
    if(i == this->owd_rle.write_index) break;
    if(++i == MPRTP_PLUGIN_MAX_RLE_LENGTH) i=0;
  }

  abs_offset -= abs_offset % GST_SECOND;
  *offset = abs_offset / GST_SECOND;

  for(i=this->owd_rle.read_index; ; )
  {
    end_seq_ = block->end_seq;
    run_type = i != this->owd_rle.write_index;
    {
      GstClockTime owd;
      owd = this->owd_rle.blocks[i].median_delay;
      owd -= abs_offset;
      //owd = GST_TIME_AS_MSECONDS(owd);
      //running_length = (owd > 0x3FFF) ? 0x3FFF : owd;
      if(owd < GST_SECOND){
        gdouble y = (gdouble) owd / (gdouble) GST_SECOND;
        y *= 16384;
        running_length = y;
      }else{
        running_length = 0x3FFF;
      }
      read = &running_length;
    }

    gst_rtcp_xr_chunk_change(chunk,
                             &chunk_type,
                             &run_type,
                             read);
    if(i == this->owd_rle.write_index) break;
    ++chunk;
    ++block;
    if(++i == MPRTP_PLUGIN_MAX_RLE_LENGTH) i=0;
  }
  THIS_READUNLOCK (this);
  if(chunks_num) *chunks_num = chunks_num_;
  if(begin_seq) *begin_seq = begin_seq_;
  if(end_seq) *end_seq = end_seq_;
  return result;
}


GstRTCPXR_Chunk *
mprtpr_path_get_lost_chunks(MpRTPRPath *this,
                              guint *chunks_num,
                              guint16 *begin_seq,
                              guint16 *end_seq)
{
  GstRTCPXR_Chunk *result = NULL, *chunk;
  gboolean chunk_type = FALSE;
  gboolean run_type;
  LostsRLEBlock *block;
  guint16 begin_seq_, end_seq_;
  gint i,chunks_num_;
  guint16 *read;

  chunks_num_ = 0;
  THIS_READLOCK (this);
  for(i=this->losts_rle.read_index; ;){
    ++chunks_num_;
    if(i == this->losts_rle.write_index) break;
    if(++i == MPRTP_PLUGIN_MAX_RLE_LENGTH) i=0;
  }
  if((chunks_num_ % 2) == 1) ++chunks_num_;
  chunk = result = mprtp_malloc(sizeof(GstRTCPXR_Chunk) * chunks_num_);
  block = &this->losts_rle.blocks[this->losts_rle.read_index];
  begin_seq_ = block->start_seq;
  for(i=this->losts_rle.read_index; ; )
  {
    end_seq_ = block->end_seq;
    run_type = i != this->losts_rle.write_index;

    read = &this->losts_rle.blocks[i].lost_packets;

    gst_rtcp_xr_chunk_change(chunk,
                             &chunk_type,
                             &run_type,
                             read);
    if(i == this->losts_rle.write_index) break;
    ++chunk;
    ++block;
    if(++i == MPRTP_PLUGIN_MAX_RLE_LENGTH) i=0;
  }
  THIS_READUNLOCK (this);
  if(chunks_num) *chunks_num = chunks_num_;
  if(begin_seq) *begin_seq = begin_seq_;
  if(end_seq) *end_seq = end_seq_;
  return result;
}



void mprtpr_path_get_joiner_stats(MpRTPRPath *this,
                           gdouble       *path_delay,
                           gdouble       *path_skew,
                           guint32       *jitter)
{
  THIS_READLOCK (this);
//  if(path_delay) *path_delay = this->estimated_delay;
  if(path_delay) *path_delay = this->delay_avg;
  if(path_skew)  *path_skew = this->path_skew; //this->estimated_skew;
  if(jitter) *jitter = this->jitter;
  THIS_READUNLOCK (this);
}

gboolean mprtpr_path_is_distorted(MpRTPRPath *this)
{
  gboolean result;
  THIS_WRITELOCK (this);
  result = this->distorted;
  this->distorted = FALSE;
  THIS_WRITEUNLOCK (this);
  return result;
}

void mprtpr_path_tick(MpRTPRPath *this)
{
  _refresh_RLE(this);
  numstracker_obsolate(this->gaps);
  numstracker_obsolate(this->lates);
}

void mprtpr_path_add_discard(MpRTPRPath *this, GstMpRTPBuffer *mprtp)
{
  THIS_WRITELOCK (this);
  _add_discard(this, mprtp);
  THIS_WRITEUNLOCK (this);
}

void mprtpr_path_add_delay(MpRTPRPath *this, GstClockTime delay)
{
  THIS_WRITELOCK (this);
  _add_delay(this, delay);
  THIS_WRITEUNLOCK (this);
}

typedef struct{
  guint64 rcv;
  guint64 snd;
  guint32 bytes;
}ActualData;

void
mprtpr_path_process_rtp_packet (MpRTPRPath * this, GstMpRTPBuffer *mprtp)
{
//  g_print("mprtpr_path_process_rtp_packet begin\n");
  gint64 skew = 0;

  THIS_WRITELOCK (this);
  if(mprtp->fec_packet){
    goto done;
  }
  if (this->seq_initialized == FALSE) {
    this->highest_seq = mprtp->subflow_seq;
    this->total_packets_received = 1;
    this->last_rtp_timestamp = mprtp->timestamp;
    this->last_mprtp_delay = mprtp->delay;
    if(0 < mprtp->delay)
      _add_delay(this, mprtp->delay);
    this->seq_initialized = TRUE;
    goto done;
  }

  skew = (((gint64)this->last_mprtp_delay - (gint64)mprtp->delay));
  this->last_mprtp_delay = mprtp->delay;
  ++this->total_packets_received;
  this->total_payload_bytes += mprtp->payload_bytes;
  this->jitter += ((skew < 0?-1*skew:skew) - this->jitter) / 16;

  if(0 < mprtp->delay){
    _add_delay(this, mprtp->delay);
    if(_cmp_seq(this->reported_sequence_number, mprtp->subflow_seq) < 0 &&
       0 < this->discard_latency &&
       this->discard_latency < mprtp->delay)
    {
      _add_discard(this, mprtp);
    }
  }
  if(_cmp_seq(mprtp->subflow_seq, this->highest_seq) <= 0){
    numstracker_add(this->lates, mprtp->subflow_seq);
    goto done;
  }
  if(_cmp_seq(this->highest_seq + 1, mprtp->subflow_seq) < 0){
    guint16 seq = this->highest_seq + 1;
    for(; _cmp_seq(seq, mprtp->subflow_seq) < 0; ++seq){
      numstracker_add(this->gaps, seq);
    }
  }else{
    if(!this->actual_group){
      this->actual_group = g_queue_new();
    }
    //packet is in order
    if(this->last_rtp_timestamp != mprtp->timestamp){
      ActualData *data;
      gdouble skew2;

      this->group_t2.rcv_avg = this->group_t1.rcv_avg;
      this->group_t2.snd_avg = this->group_t1.snd_avg;
      this->group_t1.rcv_avg = 0;
      this->group_t1.snd_avg = 0;

      while(!g_queue_is_empty(this->actual_group)){
        gdouble weight;
        data = g_queue_pop_head(this->actual_group);
        weight = (gdouble) data->bytes / (gdouble) this->actual_bytes_sum;
        this->group_t1.rcv_avg += (gdouble)data->rcv * weight;
        this->group_t1.snd_avg += (gdouble)data->snd * weight;
        g_print("this->group_t1.rcv_avg (%f) += data->rcv (%f) * weight (%f)\n", this->group_t1.rcv_avg, (gdouble)data->rcv, weight);
        g_print("this->group_t1.snd_avg (%f) += data->snd (%f) * weight (%f)\n", this->group_t1.snd_avg, (gdouble)data->snd, weight);
        mprtp_free(data);
      }
      this->actual_bytes_sum   = 0;
      this->last_rtp_timestamp = mprtp->timestamp;
      skew2  = this->group_t1.rcv_avg - this->group_t2.rcv_avg;
      g_print("A1: %f\n",this->group_t1.rcv_avg - this->group_t2.rcv_avg);
      skew2 -= this->group_t1.snd_avg - this->group_t2.snd_avg;
      g_print("A2: %f\n",this->group_t1.snd_avg - this->group_t2.snd_avg);
      skew2  = get_epoch_time_from_ntp_in_ns(skew);
      g_print("new kind of skew: %f\n", skew2);
      _add_skew(this, skew);
    }else{
      ActualData *data;
      data = mprtp_malloc(sizeof(ActualData));
      this->actual_bytes_sum += mprtp->payload_bytes;
      data->bytes = mprtp->payload_bytes;
      data->rcv   = mprtp->abs_rcv_ntp_time;
      data->snd   = mprtp->abs_snd_ntp_time;
      g_queue_push_tail(this->actual_group, data);
    }
  }

  if(65472 < this->highest_seq && mprtp->subflow_seq < 128){
    ++this->cycle_num;
  }

  this->highest_seq = mprtp->subflow_seq;

done:
  _refresh_RLEBlock(this);
  THIS_WRITEUNLOCK (this);
}

gint
_cmp_seq (guint16 x, guint16 y)
{
  if(x == y) return 0;
  if(x < y && y - x < 32768) return -1;
  if(x > y && x - y > 32768) return -1;
  if(x < y && y - x > 32768) return 1;
  if(x > y && x - y < 32768) return 1;
  return 0;
}

void _add_discard(MpRTPRPath *this, GstMpRTPBuffer *mprtp)
{
  ++this->total_late_discarded;
  this->total_late_discarded_bytes+=mprtp->payload_bytes;
  _actual_discrle(this)->discarded_bytes+=mprtp->payload_bytes;
  ++_actual_discrle(this)->discarded_packets;
  this->distorted = TRUE;
}

void _add_delay(MpRTPRPath *this, GstClockTime delay)
{
  percentiletracker_add(this->delays, delay);

  this->delay_avg = delay * .01 + this->delay_avg * .99;

  if(this->delay_avg < (delay>>1)){
    this->distorted = TRUE;
    this->request_urgent_report = TRUE;
  }

}

void _add_skew(MpRTPRPath *this, gint64 skew)
{
  GstClockTime median_skew;
  percentiletracker2_add(this->skews, skew);
  median_skew = percentiletracker2_get_stats(this->skews, NULL, NULL, NULL);
  _actual_skewrle(this)->median_delay = median_skew;
  this->path_skew = this->path_skew * .99 + (gdouble) median_skew * .01;
}

void _refresh_RLEBlock(MpRTPRPath *this)
{
  if(_cmp_seq(this->highest_seq, _actual_discrle(this)->start_seq) < 0){
    _actual_discrle(this)->start_seq = this->highest_seq;
  }
  if(_cmp_seq(_actual_discrle(this)->start_seq, this->highest_seq) < 0){
    _actual_discrle(this)->end_seq = this->highest_seq;
  }

  if(_cmp_seq(this->highest_seq, _actual_lostrle(this)->start_seq) < 0){
    _actual_lostrle(this)->start_seq = this->highest_seq;
  }
  if(_cmp_seq(_actual_lostrle(this)->start_seq, this->highest_seq) < 0){
    _actual_lostrle(this)->end_seq = this->highest_seq;
  }

  if(_cmp_seq(this->highest_seq, _actual_owdrle(this)->start_seq) < 0){
    _actual_owdrle(this)->start_seq = this->highest_seq;
  }
  if(_cmp_seq(_actual_owdrle(this)->start_seq, this->highest_seq) < 0){
    _actual_owdrle(this)->end_seq = this->highest_seq;
  }

//  g_print("RLEBlock| Begin: %hu, End: %hu| Discards: %hu| Delay: %lu\n",
//          block->start_seq,
//          block->end_seq,
//          block->discards,
//          block->median_delay);
}


void _refresh_RLE(MpRTPRPath *this)
{
  if(_discrle(this).last_step < _now(this) - _discrle(this).step_interval){
    DiscardRLE *rle;
    DiscardRLEBlock *block;
    rle = &_discrle(this);
    if(++rle->write_index == MPRTP_PLUGIN_MAX_RLE_LENGTH) rle->write_index = 0;
    rle->last_step=_now(this);
    block = _actual_discrle(this);
    memset(block, 0, sizeof(DiscardRLEBlock));
    block->start_seq = block->end_seq = this->highest_seq;
  }

  if(_lostrle(this).last_step < _now(this) - _lostrle(this).step_interval){
    LostsRLE *rle;
    LostsRLEBlock *block;
    rle = &_lostrle(this);
    if(++rle->write_index == MPRTP_PLUGIN_MAX_RLE_LENGTH) rle->write_index = 0;
    rle->last_step=_now(this);
    block = _actual_lostrle(this);
    memset(block, 0, sizeof(LostsRLEBlock));
    block->start_seq = block->end_seq = this->highest_seq;
  }

  if(_owdrle(this).last_step < _now(this) - _owdrle(this).step_interval){
    OWDRLE *rle;
    OWDRLEBlock *block;
    rle = &_owdrle(this);
    if(++rle->write_index == MPRTP_PLUGIN_MAX_RLE_LENGTH) rle->write_index = 0;
    rle->last_step=_now(this);
    block = _actual_owdrle(this);
    memset(block, 0, sizeof(OWDRLEBlock));
    block->start_seq = block->end_seq = this->highest_seq;
  }

  if(_skewrle(this).last_step < _now(this) - _skewrle(this).step_interval){
    SkewRLE *rle;
    SkewRLEBlock *block;
    rle = &_skewrle(this);
    if(++rle->write_index == MPRTP_PLUGIN_MAX_RLE_LENGTH) rle->write_index = 0;
    rle->last_step=_now(this);
    block = _actual_skewrle(this);
    memset(block, 0, sizeof(SkewRLEBlock));
    block->start_seq = block->end_seq = this->highest_seq;
  }

}

void _delays_stats_pipe(gpointer data, PercentileTrackerPipeData *pdata)
{
  MpRTPRPath *this = data;
  _actual_owdrle(this)->median_delay = pdata->percentile;
}

void _gaps_obsolation_pipe(gpointer data, gint64 seq)
{
  MpRTPRPath *this = data;
  if(numstracker_find(this->lates, seq)) return;
  ++_actual_lostrle(this)->lost_packets;
  ++this->total_packet_losts;
  this->distorted = TRUE;
}

#undef THIS_READLOCK
#undef THIS_READUNLOCK
#undef THIS_WRITELOCK
#undef THIS_WRITEUNLOCK
