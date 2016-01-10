/*
 * subratectrler.h
 *
 *  Created on: Jun 30, 2015
 *      Author: balazs
 */

#ifndef SUBRATECTRLER_H_
#define SUBRATECTRLER_H_

#include <gst/gst.h>
#include "bintree.h"

typedef struct _SubflowRateController SubflowRateController;
typedef struct _SubflowRateControllerClass SubflowRateControllerClass;

#define SUBRATECTRLER_TYPE             (subratectrler_get_type())
#define SUBRATECTRLER(src)             (G_TYPE_CHECK_INSTANCE_CAST((src),SUBRATECTRLER_TYPE,SubflowRateController))
#define SUBRATECTRLER_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST((klass),SUBRATECTRLER_TYPE,SubflowRateControllerClass))
#define SUBRATECTRLER_IS_SOURCE(src)          (G_TYPE_CHECK_INSTANCE_TYPE((src),SUBRATECTRLER_TYPE))
#define SUBRATECTRLER_IS_SOURCE_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE((klass),SUBRATECTRLER_TYPE))
#define SUBRATECTRLER_CAST(src)        ((SubflowRateController *)(src))

typedef void (*SubRateProc)(SendingRateDistributor*);

struct _SubflowRateController
{
  GObject                  object;
  GRWLock                  rwmutex;
  GstClock*                sysclock;
  guint8                   id;
  MPRTPSPath*              path;

  gint32                   requested_bytes;
  gint32                   supplied_bytes;
  gint32                   monitored_bytes;


//further development
// gdouble                target_weight;
// gboolean               bw_is_shared;
//  gdouble               weight;

  gdouble                  bounce_point;

  gint32                   max_rate;
  gint32                   min_rate;

  //Need for monitoring
  guint                    monitoring_interval;
  guint                    monitoring_time;

  SubRateProc              controller;

  guint8*                  moments;
  gint                     moments_index;
  guint32                  moments_num;

  PercentileTracker*     ltt_delays_th;
  PercentileTracker*     ltt_delays_target;

  //OWD target. Initial value: OWD_TARGET_LO
  guint64                owd_target;
  //EWMA filtered owd fraction.Initial value:  0.0
  gdouble                owd_fraction_avg;
  //Vector of the last 20 owd_fraction
  FloatNumsTracker      *owd_fraction_hist;
  //OWD trend indicates incipient congestion. Initial value: 0.0
  gdouble                owd_trend_mem;
  //True if in fast start state. Initial value: TRUE
  gint                   in_fast_start;
  gint                   n_fast_start;
  //Maximum segment size
  gint                   mss;
  //Minimum congestion window [byte]. Initial value: 2*MSS
  gint                   cwnd_min;
  //COngestion window
  gint                   cwnd;
  //Congestion window inflection point. Initial value: 1
  gint                   cwnd_i;
  GstClockTime           last_congestion_detected;

  //Skip encoding of video frame if true. gint false
  gboolean               frame_skip;
  //Indicates the intensity of the frame skips. Initial value: 0.0
  gdouble                frame_skip_intensity;
  //Number of video  frames since the last skip.Initial value:  0
  gint                   since_last_frame_skip;
  //Number of consecutive frame skips.Initial value:  0
  gint                   consecutive_frame_skips;
  //Video target bitrate [bps]
  gint                   target_bitrate;
  //Video target bitrate inflection point i.e. the last known highest
  //target_bitrate during fast start. Used to limit bitrate increase
  //close to the last know congestion point. Initial value: 1
  gint                   target_bitrate_i;
  //Measured transmit bitrate [bps]. Initial value: 0.0
  gdouble                rate_transmit;
  //Measured throughput based on received acknowledgements [bps].
  //Initial value: 0.0
  gdouble                rate_acked;
  //Smoothed RTT [s], computed similar to method depicted in [RFC6298].
  //Initial value: 0.0
  gdouble                s_rtt;
  //Size of RTP packets in queue [bits].
  gint                   rtp_queue_size;
  //Size of the last transmitted RTP packets [byte]. Initial value: 0
  gint                   rtp_size;

};


struct _SubflowRateControllerClass{
  GObjectClass parent_class;

};
GType subratectrler_get_type (void);
SubflowRateController *make_subratectrler(MPRTPSPath *path);
void subratectrler_set(SubflowRateController *this,
                              MPRTPSPath *path,
                              guint32 sending_target);
void subratectrler_unset(SubflowRateController *this);
guint64 subratectrler_measurement_update(
                         SubflowRateController *this,
                         RRMeasurement * measurement);

#endif /* SUBRATECTRLER_H_ */