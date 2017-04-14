/*
 * fractalfbprocessor.h
 *
 *  Created on: Jun 30, 2015
 *      Author: balazs
 */

#ifndef FRACTALFBPROCESSOR_H_
#define FRACTALFBPROCESSOR_H_

#include <gst/gst.h>

#include "lib_swplugins.h"
#include "notifier.h"
#include "sndtracker.h"
#include "sndsubflows.h"
#include "reportproc.h"


typedef struct _FRACTaLFBProcessor FRACTaLFBProcessor;
typedef struct _FRACTaLFBProcessorClass FRACTaLFBProcessorClass;

#define FRACTALFBPROCESSOR_TYPE             (fractalfbprocessor_get_type())
#define FRACTALFBPROCESSOR(src)             (G_TYPE_CHECK_INSTANCE_CAST((src),FRACTALFBPROCESSOR_TYPE,FRACTaLFBProcessor))
#define FRACTALFBPROCESSOR_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST((klass),FRACTALFBPROCESSOR_TYPE,FRACTaLFBProcessorClass))
#define FRACTALFBPROCESSOR_IS_SOURCE(src)          (G_TYPE_CHECK_INSTANCE_TYPE((src),FRACTALFBPROCESSOR_TYPE))
#define FRACTALFBPROCESSOR_IS_SOURCE_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE((klass),FRACTALFBPROCESSOR_TYPE))
#define FRACTALFBPROCESSOR_CAST(src)        ((FRACTALFBProcessor *)(src))

typedef struct _FRACTaLStat
{
  GstClockTime             jitter_50th;
  GstClockTime             queue_delay;
  GstClockTime             last_drift;

  GstClockTime             last_raise;
  GstClockTime             raise_avg;
  GstClockTime             raise_std;
  GstClockTime             last_fall;
  GstClockTime             fall_avg;
  GstClockTime             fall_std;

  gint32                   measurements_num;
  gint32                   BiF_80th;
  gint32                   BiF_max;
  gint32                   BiF_min;
  gint32                   BiF_std;
  gdouble                  FL_std;
  gint32                   extra_bytes;
  gint32                   bytes_in_flight;
  gdouble                  rtpq_delay;
  gint32                   sender_bitrate;
  gint32                   receiver_bitrate;
  gint32                   fec_bitrate;

  GstClockTime             drift_avg;
  GstClockTime             drift_std;
  gdouble                  srtt;

  gint32                   newly_received_bytes;
  gint32                   newly_lost_packets;

  gdouble                  sr_avg;
  gdouble                  rr_avg;

  gdouble                  FL_in_1s;
  gdouble                  FL_avg;

  gdouble                  est_receiver_rate;
  gint32                   acked_bytes_in_srtt;

  gdouble                  last_FL;

//  gdouble                  BiF_SR_corr_avg;

}FRACTaLStat;

typedef struct{
  guint   counter;
  gdouble mean;
  gdouble var;
  gdouble emp;

  gdouble sum;
}FRACTaLStdHelper;


typedef struct{
  gint32       ref;
  GstClockTime queue_delay;
  GstClockTime flaw;
  gint32       bytes_in_flight;
  gdouble      fraction_lost;
  gint8        stability;
  gdouble      qdelay_std_t;
  gint32       newly_received_bytes;
}FRACTaLMeasurement;

typedef struct{
  gboolean owd;
  gboolean bytes_in_flight;
  gboolean fraction_lost;
}FRACTaLApprovement;

struct _FRACTaLFBProcessor
{
  GObject                  object;
  GstClock*                sysclock;

  SlidingWindow*           srtt_sw;
  SlidingWindow*           short_sw;
  SlidingWindow*           long_sw;
  Recycle*                 measurements_recycle;

  FRACTaLStat*             stat;
  FRACTaLApprovement*      approvement;
  SndTracker*              sndtracker;
  SndSubflow*              subflow;

  guint                    rcved_fb_since_changed;
  gint32                   last_bytes_in_flight;
  GstClockTime             RTT;
  GstClockTime             srtt_updated;

  GstClockTime             jitter_min;
  GstClockTime             jitter_max;

  gdouble                  FL_min;
  gdouble                  FL_max;
  gint32                   acked_bytes_in_srtt;

//  struct{
//   gdouble sum;
//   guint32 count;
//   gdouble avg;
//  }raise_stat,fall_stat;

  FRACTaLStdHelper         flaw_std_helper;
  FRACTaLStdHelper         BiF_std_helper;
  FRACTaLStdHelper         FL_std_helper;
  GstClockTime             last_report_updated;
  GstClockTime             last_owd_log;

  gint32                   newly_acked_packets;
  gint32                   newly_received_packets;
  guint16                  HSN;

  gint64                  last_raise;
  gint64                  last_fall;
  gint64                  queue_delay;

};

struct _FRACTaLFBProcessorClass{
  GObjectClass parent_class;

};

GType fractalfbprocessor_get_type (void);
FRACTaLFBProcessor *make_fractalfbprocessor(SndTracker* sndtracker, SndSubflow* subflow,
    FRACTaLStat* stat, FRACTaLApprovement* approvements);

void fractalfbprocessor_reset(FRACTaLFBProcessor *this);
gint32 fractalfbprocessor_get_estimation(FRACTaLFBProcessor *this);
void fractalfbprocessor_start_estimation(FRACTaLFBProcessor *this);
void fractalfbprocessor_reset_short_sw(FRACTaLFBProcessor *this);
void fractalfbprocessor_approve_measurement(FRACTaLFBProcessor *this);
void fractalfbprocessor_time_update(FRACTaLFBProcessor *this);
void fractalfbprocessor_report_update(FRACTaLFBProcessor *this, GstMPRTCPReportSummary *summary);

#endif /* FRACTALFBPROCESSOR_H_ */
