/*
 * smanctrler.h
 *
 *  Created on: Jun 30, 2015
 *      Author: balazs
 */

#ifndef STREAM_JOINER_H_
#define STREAM_JOINER_H_

#include <gst/gst.h>

typedef struct _StreamJoiner StreamJoiner;
typedef struct _StreamJoinerClass StreamJoinerClass;

#include "mprtprpath.h"

#define STREAM_JOINER_TYPE             (stream_joiner_get_type())
#define STREAM_JOINER(src)             (G_TYPE_CHECK_INSTANCE_CAST((src),STREAM_JOINER_TYPE,StreamJoiner))
#define STREAM_JOINER_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST((klass),STREAM_JOINER_TYPE,StreamJoinerClass))
#define STREAM_JOINER_IS_SOURCE(src)          (G_TYPE_CHECK_INSTANCE_TYPE((src),STREAM_JOINER_TYPE))
#define STREAM_JOINER_IS_SOURCE_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE((klass),STREAM_JOINER_TYPE))
#define STREAM_JOINER_CAST(src)        ((StreamJoiner *)(src))

#define MPRTP_SENDER_SCHTREE_MAX_PATH_NUM 32
#define MAX_SKEWS_ARRAY_LENGTH 256

typedef struct _FrameNode FrameNode;
typedef struct _Frame Frame;


struct _StreamJoiner
{
  GObject               object;

  GstTask*             thread;
  GRecMutex            thread_mutex;
  GHashTable*          subflows;
  GRWLock              rwmutex;
  guint8               monitor_payload_type;
  gboolean             playout_allowed;
  gboolean             playout_halt;
  GstClockTime         playout_halt_time;
  GstClockTime         stream_delay;
  gint                 subflow_num;
  GstClock*            sysclock;
  GstClockTime         last_obsolate_checked;
//  gdouble              latency;
  gdouble              playout_delay;
  NumsTracker*         skews;
  NumsTracker*         delays;
  gint64               max_skew;
  GstClockTime         max_delay_diff;
  GstClockTime         max_delay;
  GstClockTime         min_delay;
  GstClockTime         forced_delay;
//  PercentileTracker*   latency_window;
  guint32              ssrc;
//  PointerPool*         frames_pool;
//  PointerPool*         framenodes_pool;
  Frame*               head;
  Frame*               tail;
  guint16              PHSN;
  gint32               bytes_in_queue;
  guint32              last_played_timestamp;
  gboolean             flushing;
//  SKalmanFilter*       tick_estimator;
  PercentileTracker*   ticks;
  gdouble              estimated_tick;
  gint32               framecounter;

  guint64              last_snd_ntp_reference;
  void               (*send_mprtp_packet_func)(gpointer,GstMpRTPBuffer*);
  gpointer             send_mprtp_packet_data;
};
struct _StreamJoinerClass{
  GObjectClass parent_class;
};

StreamJoiner*
make_stream_joiner(
    gpointer data,
    void (*func)(gpointer,GstMpRTPBuffer*));

void
stream_joiner_add_path(
    StreamJoiner * this,
    guint8 subflow_id,
    MpRTPRPath *path);

void
stream_joiner_rem_path(
    StreamJoiner * this,
    guint8 subflow_id);

void
stream_joiner_receive_mprtp(
    StreamJoiner * this,
    GstMpRTPBuffer *rtp);

void
stream_joiner_set_monitor_payload_type(
    StreamJoiner *this,
    guint8 monitor_payload_type);

void
stream_joiner_get_stats(
    StreamJoiner *this,
    gdouble *playout_delay,
    gint32 *playout_buffer_size);

void
stream_joiner_set_playout_halt_time(
    StreamJoiner *this,
    GstClockTime halt_time);

void
stream_joiner_set_playout_allowed(
    StreamJoiner *this,
    gboolean playout_permission);

void
stream_joiner_set_forced_delay(
    StreamJoiner *this,
    GstClockTime tick_interval);

void
stream_joiner_set_stream_delay(
    StreamJoiner *this,
    GstClockTime stream_delay);

guint32
stream_joiner_get_monitored_bytes(
    StreamJoiner *this,
    guint8 subflow_id);

GType
stream_joiner_get_type (void);

#endif /* STREAM_JOINER_H_ */
