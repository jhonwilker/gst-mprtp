/*
 * packetstracker.h
 *
 *  Created on: Jun 30, 2015
 *      Author: balazs
 */

#ifndef PACKETSTRACKER_H_
#define PACKETSTRACKER_H_

#include <gst/gst.h>
#include "reportproc.h"

typedef struct _PacketsTracker PacketsTracker;
typedef struct _PacketsTrackerClass PacketsTrackerClass;

#define PACKETSTRACKER_TYPE             (packetstracker_get_type())
#define PACKETSTRACKER(src)             (G_TYPE_CHECK_INSTANCE_CAST((src),PACKETSTRACKER_TYPE,PacketsTracker))
#define PACKETSTRACKER_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST((klass),PACKETSTRACKER_TYPE,PacketsTrackerClass))
#define PACKETSTRACKER_IS_SOURCE(src)          (G_TYPE_CHECK_INSTANCE_TYPE((src),PACKETSTRACKER_TYPE))
#define PACKETSTRACKER_IS_SOURCE_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE((klass),PACKETSTRACKER_TYPE))
#define PACKETSTRACKER_CAST(src)        ((PacketsTracker *)(src))


typedef struct _PacketsTrackerStat
{
  gint32                   bytes_in_flight;
  gint32                   acked_in_1s;
  gint32                   received_in_1s;
  gint32                   goodput_in_1s;
  gint32                   sent_in_1s;
}PacketsTrackerStat;

struct _PacketsTracker
{
  GObject                  object;
  GRWLock                  rwmutex;
  GstClock*                sysclock;
  GQueue*                  sent;
  GQueue*                  sent_in_1s;
  GQueue*                  acked;

  gint32                   bytes_in_flight;
  gint32                   acked_in_1s;
  gint32                   received_in_1s;
  gint32                   goodput_in_1s;
  gint32                   sent_in_1s_sum;

};

struct _PacketsTrackerClass{
  GObjectClass parent_class;

};

GType packetstracker_get_type (void);
PacketsTracker *make_packetstracker(void);

void packetstracker_reset(PacketsTracker *this);
void packetstracker_add(PacketsTracker *this, GstRTPBuffer *rtp, guint16 sn);
void packetstracker_get_stats (PacketsTracker * this, PacketsTrackerStat* result);
void packetstracker_feedback_update(PacketsTracker *this, GstMPRTCPReportSummary *summary);

#endif /* PACKETSTRACKER_H_ */