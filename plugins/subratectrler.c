/* GStreamer
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
#include "subratectrler.h"
#include "gstmprtcpbuffer.h"
#include <math.h>
#include <string.h>
#include "bintree.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

//#define THIS_READLOCK(this) g_rw_lock_reader_lock(&this->rwmutex)
//#define THIS_READUNLOCK(this) g_rw_lock_reader_unlock(&this->rwmutex)
//#define THIS_WRITELOCK(this) g_rw_lock_writer_lock(&this->rwmutex)
//#define THIS_WRITEUNLOCK(this) g_rw_lock_writer_unlock(&this->rwmutex)

#define THIS_READLOCK(this)
#define THIS_READUNLOCK(this)
#define THIS_WRITELOCK(this)
#define THIS_WRITEUNLOCK(this)

GST_DEBUG_CATEGORY_STATIC (subratectrler_debug_category);
#define GST_CAT_DEFAULT subratectrler_debug_category

G_DEFINE_TYPE (SubflowRateController, subratectrler, G_TYPE_OBJECT);

#define MOMENTS_LENGTH 8
#define KEEP_MAX 20
#define KEEP_MIN 5

#define DEFAULT_RAMP_UP_AGGRESSIVITY 0.
#define DEFAULT_DISCARD_AGGRESSIVITY .1
// Min OWD target. Default value: 0.1 -> 100ms
#define OWD_TARGET_LO 100 * GST_MSECOND
//Max OWD target. Default value: 0.4s -> 400ms
#define OWD_TARGET_HI 400 * GST_MSECOND
// Max video rampup speed in bps/s (bits per second increase per second)
#define RAMP_UP_MAX_SPEED 200000.0f // bps/s
#define RAMP_UP_MIN_SPEED 2000.0f // bps/s
//CWND scale factor due to loss event. Default value: 0.6
#define BETA 0.6
// Target rate scale factor due to loss event. Default value: 0.8
#define BETA_R 0.8
//Interval between video bitrate adjustments. Default value: 0.2s ->200ms
#define RATE_ADJUST_INTERVAL 200 * GST_MSECOND /* ms */
//Min target_bitrate [bps]
//#define TARGET_BITRATE_MIN 500000
#define TARGET_BITRATE_MIN SUBFLOW_DEFAULT_SENDING_RATE / 2
//Max target_bitrate [bps]
#define TARGET_BITRATE_MAX 0
//Timespan [s] from lowest to highest bitrate. Default value: 10s->10000ms
#define RAMP_UP_TIME 10000
//Guard factor against early congestion onset.
//A higher value gives less jitter possibly at the
//expense of a lower video bitrate. Default value: 0.0..0.95
#define PRE_CONGESTION_GUARD .0
//Guard factor against RTP queue buildup. Default value: 0.0..2.0
#define TX_QUEUE_SIZE_FACTOR 1.0

typedef struct _Moment Moment;

typedef enum{
  EVENT_CONGESTION           = -2,
  EVENT_DISTORTION           = -1,
  EVENT_FI                   =  0,
  EVENT_SETTLED              =  1,
  EVENT_STEADY               =  2,
  EVENT_PROBE                =  3,
}Event;

typedef enum{
  STATE_OVERUSED       = -1,
  STATE_STABLE         =  0,
  STATE_MONITORED      =  1,
}State;

typedef enum{
  STAGE_REDUCE            = -3,
  STAGE_BOUNCE            = -2,
  STAGE_MITIGATE          = -1,
  STAGE_KEEP              =  0,
  STAGE_RAISE             =  1,
}Stage;

struct _Moment{
  GstClockTime      time;
  //Explicit Congestion Notifier Influence Values (ECN)
  gboolean          lost;
  gboolean          discard;
  gboolean          recent_discard;
  gboolean          recent_lost;
  gboolean          path_is_congested;
  gboolean          path_is_lossy;
  gboolean          tr_is_mitigated;
  gint32            has_expected_lost;
  gint32            sender_bitrate;
  gint32            goodput_bitrate;
  gint32            receiver_bitrate;
  gdouble           discard_rate;
  SubAnalyserResult analysation;
  gboolean          mitigated;
  gdouble           rtt;

  //application
  Event             event;
  State             state;
  Stage             stage;
  gboolean          controlled;

};



//----------------------------------------------------------------------
//-------- Private functions belongs to Scheduler tree object ----------
//----------------------------------------------------------------------

 void subratectrler_finalize (GObject * object);


// const gdouble ST_ = 1.1; //Stable treshold
// const gdouble OT_ = 2.;  //Overused treshold
// const gdouble DT_ = 1.5; //Down Treshold


//--------------------MEASUREMENTS-----------------------

#define _ramp_up_speed(this) (MIN(RAMP_UP_SPEED, this->target_bitrate))
#define _now(this) (gst_clock_get_time(this->sysclock))
//#define _get_moment(this, n) ((Moment*)(this->moments + n * sizeof(Moment)))
static Moment* _get_moment(SubflowRateController *this, gint n)
{
  Moment* moments = this->moments;
  return moments + this->moments_index;
}
#define _pmtn(index) (index == MOMENTS_LENGTH ? MOMENTS_LENGTH - 1 : index == 0 ? MOMENTS_LENGTH - 1 : index - 1)
#define _mt0(this) _get_moment(this, this->moments_index)
#define _mt1(this) _get_moment(this, _pmtn(this->moments_index))
#define _mt2(this) _get_moment(this, _pmtn(_pmtn(this->moments_index)))
#define _mt3(this) _get_moment(this, _pmtn(_pmtn(_pmtn(this->moments_index))))

#define _anres(this) _mt0(this)->analysation
#define _anres_t1(this) _mt1(this)->analysation
#define _anres_t2(this) _mt2(this)->analysation
#define _delays(this) _anres(this).delay_indicators
#define _bitrate(this) _anres(this).rate_indicators
#define _delays_t1(this) _anres_t1(this).delay_indicators
#define _delays_t2(this) _anres_t2(this).delay_indicators
#define _bitrate_t1(this) _anres_t1(this).rate_indicators
#define _bitrate_t2(this) _anres_t2(this).rate_indicators
//#define _reset_target_points(this) numstracker_reset(this->target_points)
#define _skip_frames_for(this, duration) mprtps_path_set_skip_duration(this->path, duration);
#define _qtrend(this) _anres(this).qtrend
#define _qtrend_th(this) 1.05
#define _qtrend_th2(this) 1.5
#define _deoff(this) _anres(this).delay_off
#define _rdiscard(this) (0 < _mt0(this)->recent_discard)
#define _discard(this) (this->discard_aggressivity < _anres(this).discards_rate)
#define _lost(this)  ((_mt0(this)->has_expected_lost ? FALSE : _mt0(this)->path_is_lossy ? FALSE : _mt0(this)->lost > 0))
#define _rlost(this) (!_mt0(this)->has_expected_lost && !_mt0(this)->path_is_lossy && _mt0(this)->recent_lost > 0)
#define _state_t1(this) _mt1(this)->state
#define _state_t2(this) _mt2(this)->state
#define _state(this) _mt0(this)->state
#define _stage(this) _mt0(this)->stage
#define _stage_t1(this) _mt1(this)->stage
#define _stage_t2(this) _mt2(this)->stage
#define _stage_t3(this) _mt3(this)->stage
#define _TR(this) this->target_bitrate
#define _SR(this) _anres(this).sending_rate_median
#define _RR(this) _mt0(this)->receiver_bitrate
#define _RR_t1(this) _mt1(this)->receiver_bitrate
#define _GP(this) _mt0(this)->goodput_bitrate
#define _GP_t1(this) _mt1(this)->goodput_bitrate
#define _min_br(this) MIN(_SR(this), _TR(this))
#define _max_br(this) MAX(_SR(this), _TR(this))
#define _set_pending_event(this, e) this->pending_event = e
#define _set_event(this, e) _mt0(this)->event = e
#define _event(this) _mt0(this)->event
#define _event_t1(this) _mt1(this)->event


 static Moment*
_m_step(
    SubflowRateController *this);

static void
_reduce_stage(
    SubflowRateController *this);

static void
_bounce_stage(
    SubflowRateController *this);

static void
_keep_stage(
    SubflowRateController *this);

static void
_mitigate_stage(
    SubflowRateController *this);

static void
_raise_stage(
    SubflowRateController *this);

static void
_switch_stage_to(
    SubflowRateController *this,
    Stage target,
    gboolean execute);

#define _transit_state_to(this, target)   _mt0(this)->state = target;

static void
_fire(
    SubflowRateController *this,
    Event event);

#define _enable_pacing(this) _set_path_pacing(this, TRUE)
#define _disable_pacing(this) _set_path_pacing(this, FALSE)
#define _pacing_enabled(this) this->path_is_paced

static void
_disable_controlling(
    SubflowRateController *this);

#define MAX_MONITORING_INTERVAL 14
#define MIN_PROBE_MONITORING_INTERVAL 5
#define MIN_BOUNCE_MONITORING_INTERVAL 2
#define MAX_MONITORING_RATE 200000
#define _disable_monitoring(this) _set_monitoring_interval(this, 0)

static void
_reset_monitoring(
    SubflowRateController *this);

static void
_setup_bounce_monitoring(
    SubflowRateController *this);

static void
_setup_raise_monitoring(
    SubflowRateController *this);

static void
_set_monitoring_interval(
    SubflowRateController *this,
    guint interval);

static guint
_calculate_monitoring_interval(
    SubflowRateController *this,
    guint32 desired_bitrate);

static void
_change_target_bitrate(SubflowRateController *this, gint32 new_target);

 static gboolean
 _is_near_to_bottleneck_point(
     SubflowRateController *this);

 static gdouble
 _get_bottleneck_influence(
     SubflowRateController *this);

static void
_add_bottleneck_point(
    SubflowRateController *this,
    gint32 rate);

static void
_append_to_log(
    SubflowRateController *this,
    const gchar * format,
    ...);

static void
_log_measurement_update_state(
    SubflowRateController *this);

static void
_log_abbrevations(
    SubflowRateController *this,
    FILE *file);


//----------------------------------------------------------------------
//--------- Private functions implementations to SchTree object --------
//----------------------------------------------------------------------

void
subratectrler_class_init (SubflowRateControllerClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = (GObjectClass *) klass;

  gobject_class->finalize = subratectrler_finalize;

  GST_DEBUG_CATEGORY_INIT (subratectrler_debug_category, "subratectrler", 0,
      "MpRTP Manual Sending Controller");

}

void
subratectrler_finalize (GObject * object)
{
  SubflowRateController *this;
  this = SUBRATECTRLER(object);
  g_free(this->moments);
  g_object_unref(this->analyser);
  g_object_unref(this->sysclock);
}

void
subratectrler_init (SubflowRateController * this)
{
  this->moments = g_malloc0(sizeof(Moment) * MOMENTS_LENGTH);
  this->sysclock = gst_system_clock_obtain();
  this->analyser = make_subanalyser();
  g_rw_lock_init (&this->rwmutex);
}


SubflowRateController *make_subratectrler(SendingRateDistributor* rate_controlller)
{
  SubflowRateController *result;
  result = g_object_new (SUBRATECTRLER_TYPE, NULL);
  result->rate_controller = rate_controlller;
  return result;
}

void subratectrler_enable_logging(SubflowRateController *this,
                                                    const gchar *filename)
{
  FILE *file;
  THIS_WRITELOCK(this);
  this->log_enabled = TRUE;
  this->logtick = 0;
  strcpy( this->log_filename, filename);
  file = fopen(this->log_filename, "w");
  _log_abbrevations(this, file);
  fclose(file);
  THIS_WRITEUNLOCK(this);
}

void subratectrler_disable_logging(SubflowRateController *this)
{
  THIS_WRITELOCK(this);
  this->log_enabled = FALSE;
  THIS_WRITEUNLOCK(this);
}

void subratectrler_set(SubflowRateController *this,
                         MPRTPSPath *path,
                         guint32 sending_target,
                         guint64 initial_disabling)
{
  THIS_WRITELOCK(this);
  this->path = g_object_ref(path);
  memset(this->moments, 0, sizeof(Moment) * MOMENTS_LENGTH);

  this->id = mprtps_path_get_id(this->path);
  this->setup_time = _now(this);
  this->monitoring_interval = 3;
  this->target_bitrate = sending_target;
  this->min_rate = TARGET_BITRATE_MIN;
  this->max_rate = TARGET_BITRATE_MAX;
  this->min_target_point = MIN(TARGET_BITRATE_MIN, sending_target);
  this->max_target_point = MAX(TARGET_BITRATE_MAX, sending_target * 2);
  this->discard_aggressivity = DEFAULT_DISCARD_AGGRESSIVITY;
  this->ramp_up_aggressivity = DEFAULT_RAMP_UP_AGGRESSIVITY;
  this->keep = KEEP_MIN;

  _switch_stage_to(this, STAGE_KEEP, FALSE);
  _transit_state_to(this, STATE_STABLE);
  subanalyser_reset(this->analyser);
  if(initial_disabling < 10  * GST_SECOND){
    this->disable_controlling = _now(this) + initial_disabling;
  }else{
    this->disable_controlling = _now(this) + initial_disabling;
  }
  THIS_WRITEUNLOCK(this);
}

void subratectrler_unset(SubflowRateController *this)
{
  THIS_WRITELOCK(this);
  g_object_unref(this->path);
  this->path = NULL;
  THIS_WRITEUNLOCK(this);
}



void subratectrler_measurement_update(
                         SubflowRateController *this,
                         RRMeasurement * measurement)
{
  struct _SubflowUtilizationReport *report = &this->utilization.report;
  if(measurement->goodput <= 0.) goto done;
  _m_step(this);

  _mt0(this)->time                = measurement->time;
//  _mt0(this)->lost                = measurement->lost;
  _mt0(this)->lost                = measurement->rfc3611_cum_lost;
  _mt0(this)->recent_lost         = measurement->recent_lost;
  _mt0(this)->recent_discard      = measurement->recent_discarded_bytes;
  _mt0(this)->path_is_lossy       = !mprtps_path_is_non_lossy(this->path);
  _mt0(this)->has_expected_lost   = _mt1(this)->has_expected_lost;
  _mt0(this)->state               = _mt1(this)->state;
  _mt0(this)->stage               = _mt1(this)->stage;
  _mt0(this)->discard_rate        = measurement->goodput / measurement->receiver_rate;
  _mt0(this)->receiver_bitrate    = measurement->receiver_rate * 8;
  _mt0(this)->sender_bitrate      = measurement->sender_rate * 8;
  _mt0(this)->goodput_bitrate     = measurement->goodput * 8.;
  _mt0(this)->event               = EVENT_FI;
  _mt0(this)->state               = _mt1(this)->state;
  _mt0(this)->rtt                 = (gdouble)measurement->RTT / (gdouble) GST_SECOND;

  if(measurement->expected_lost)
    _mt0(this)->has_expected_lost   = 3;
  else if(_mt0(this)->has_expected_lost > 0)
    --_mt0(this)->has_expected_lost;

  subanalyser_measurement_analyse(this->analyser,
                                  measurement,
                                  _TR(this),
                                  _mt0(this)->sender_bitrate,
                                  &_mt0(this)->analysation);

  if(0 < this->disable_controlling && this->disable_controlling < _now(this)){
      this->disable_controlling = 0;
  }
  if(5 < this->moments_num && this->disable_controlling == 0LU){
    if(this->pending_event != EVENT_FI){
      _fire(this, this->pending_event);
      this->pending_event = EVENT_FI;
    }
    //Execute stage
    this->stage_fnc(this);
    _fire(this, _event(this));
    _mt0(this)->controlled = TRUE;
  }

  _log_measurement_update_state(this);

  if(_state(this) != STATE_OVERUSED && _state_t1(this) == STATE_OVERUSED){
    _mt0(this)->has_expected_lost   = 3;
  }
//  if(_mt0(this)->state != STATE_OVERUSED || this->last_reference_added < _now(this) - 10 * GST_SECOND){
//    subanalyser_measurement_add_to_reference(this->analyser, measurement);
//    this->last_reference_added = _now(this);
//  }
done:
  report->discarded_bytes = 0; //Fixme
  report->lost_bytes = 0;//Fixme
  report->sending_rate = _SR(this);
  report->target_rate = _TR(this);
  report->state = _state(this);
  report->rtt  = _mt0(this)->rtt;
  sndrate_setup_report(this->rate_controller, this->id, report);
  return;
}

void subratectrler_setup_controls(
                         SubflowRateController *this, struct _SubflowUtilizationControl* src)
{
  struct _SubflowUtilizationControl *ctrl = &this->utilization.control;
  memcpy(ctrl, src, sizeof(struct _SubflowUtilizationControl));
  this->min_rate = ctrl->min_rate;
  this->max_rate = ctrl->max_rate;
  if(this->target_bitrate < this->min_rate) this->target_bitrate = this->min_rate;
}

Moment* _m_step(SubflowRateController *this)
{
  if(++this->moments_index == MOMENTS_LENGTH){
    this->moments_index = 0;
  }
  ++this->moments_num;
  memset(_mt0(this), 0, sizeof(Moment));
  return _mt0(this);
}


//void
//_reduce_stage(
//    SubflowRateController *this)
//{
//  gint32 dSR, NSR , max_rr/*New SR*/;
//  max_rr = MAX(_RR(this), _RR_t1(this));
//  this->bottleneck_point = MIN(_TR(this), max_rr);
//  if(_SR(this) <= max_rr){
//    NSR = .9 * _SR(this);
//    goto done;
//  }
//  dSR = _SR(this) - max_rr;
//  g_print("DELTA SR: %d\n", dSR);
//  NSR = MAX(.6 * _SR(this), _SR(this) - 2 * dSR);
//done:
//  _change_target_bitrate(this, NSR);
//  _switch_stage_to(this, STAGE_BOUNCE, FALSE);
//}

void
_reduce_stage(
    SubflowRateController *this)
{
  if(!this->reduced || 3 < this->in_congestion){
    _change_target_bitrate(this, _min_br(this) * .6);
    this->reduced = TRUE;
    this->in_congestion = 0;
    goto done;
  }
  if(_anres(this).congested){
    ++this->in_congestion;
    goto done;
  }
  this->in_congestion = 0;
  _switch_stage_to(this, STAGE_BOUNCE, FALSE);
done:
  return;
}

void
_bounce_stage(
    SubflowRateController *this)
{
  if(_anres(this).congested){
    this->bottleneck_point = _TR(this) * .9;
    _switch_stage_to(this, STAGE_REDUCE, TRUE);
    _set_event(this, EVENT_CONGESTION);
    goto exit;
  }

  _change_target_bitrate(this, this->bottleneck_point);
  _switch_stage_to(this, STAGE_KEEP, FALSE);
  _set_pending_event(this, EVENT_SETTLED);
  this->keep = KEEP_MIN;
exit:
  return;
}

void
_keep_stage(
    SubflowRateController *this)
{
  gint32 target_rate = _TR(this);

  if(_anres(this).distorted){
    _switch_stage_to(this, STAGE_MITIGATE, TRUE);
    _set_event(this, EVENT_DISTORTION);
    goto exit;
  }

  if(_anres(this).pierced){
    this->keep = MIN(this->keep * 2, KEEP_MAX);
    goto done;
  }

  if(_anres(this).stable < this->keep || !_anres(this).tr_correlated){
    goto done;
  }

  if(0 < this->bottleneck_point && target_rate < this->bottleneck_point){
    target_rate = this->bottleneck_point;
    subanalyser_reset(this->analyser);
    goto done;
  }


  _switch_stage_to(this, STAGE_RAISE, FALSE);
  _set_event(this, EVENT_PROBE);
done:
  _change_target_bitrate(this, target_rate);
exit:
  return;
}

void
_mitigate_stage(
    SubflowRateController *this)
{
  gint32 target_rate = _TR(this);
  this->congestion_detected = _now(this);
  if(_anres(this).congested || _lost(this)){
    _switch_stage_to(this, STAGE_REDUCE, TRUE);
    _set_event(this, EVENT_CONGESTION);
    goto exit;
  }

  if(_mt1(this)->mitigated){
    goto exit;
  }
  this->bottleneck_point = _max_br(this) * .9;
  target_rate = this->bottleneck_point * .9;

  _change_target_bitrate(this, target_rate);
  _set_pending_event(this, EVENT_SETTLED);
  _switch_stage_to(this, STAGE_KEEP, FALSE);
  this->keep = MAX(KEEP_MAX / 2, this->keep);
  _mt0(this)->mitigated = TRUE;
exit:
  return;
}


void
_raise_stage(
    SubflowRateController *this)
{
  gint32 target_rate = _TR(this);

  if(_anres(this).distorted || _lost(this)){
    _switch_stage_to(this, STAGE_MITIGATE, TRUE);
    _set_event(this, EVENT_DISTORTION);
    goto exit;
  }

  if(_anres(this).pierced){
    target_rate = this->bottleneck_point;
    this->keep = MAX(KEEP_MIN * 2, this->keep);
    _switch_stage_to(this, STAGE_KEEP, FALSE);
    _set_pending_event(this, EVENT_STEADY);
    _disable_monitoring(this);
    goto exit;
  }

  if(_anres(this).stable < this->keep || !_anres(this).tr_correlated){
    goto done;
  }

  this->bottleneck_point = MAX(target_rate * .9, this->bottleneck_point);
  if(this->target_bitrate < this->desired_bitrate){
    target_rate  = target_rate + CONSTRAIN(RAMP_UP_MIN_SPEED,
                                           RAMP_UP_MAX_SPEED,
                                           this->desired_bitrate - this->target_bitrate);
    subanalyser_reset_stability(this->analyser);
    goto done;
  }

  this->keep = MAX(this->keep / 2, KEEP_MIN);
  _switch_stage_to(this, STAGE_KEEP, FALSE);
  _set_pending_event(this, EVENT_STEADY);
  _disable_monitoring(this);
done:
  _change_target_bitrate(this, target_rate);
exit:
  return;
}

void
_fire(
    SubflowRateController *this,
    Event event)
{
  switch(_state(this)){
    case STATE_OVERUSED:
      switch(event){
        case EVENT_CONGESTION:
          this->reduced = FALSE;
          mprtps_path_set_congested(this->path);
          _disable_controlling(this);
          _disable_monitoring(this);
        break;
        case EVENT_SETTLED:
          mprtps_path_set_non_congested(this->path);
          _disable_monitoring(this);
          _reset_monitoring(this);
          _transit_state_to(this, STATE_STABLE);
        break;
        case EVENT_PROBE:
          _setup_bounce_monitoring(this);
        break;
        case EVENT_FI:
        default:
        break;
      }
    break;
    case STATE_STABLE:
      switch(event){
        case EVENT_CONGESTION:
          this->reduced = FALSE;
          mprtps_path_set_congested(this->path);
          _transit_state_to(this, STATE_OVERUSED);
          _disable_controlling(this);
        break;
        case EVENT_DISTORTION:
          _transit_state_to(this, STATE_OVERUSED);
          break;
        case EVENT_PROBE:
          _transit_state_to(this, STATE_MONITORED);
          _setup_raise_monitoring(this);
        break;
        case EVENT_FI:
        default:
        break;
      }
    break;
    case STATE_MONITORED:
      switch(event){
        case EVENT_CONGESTION:
          this->reduced = FALSE;
          mprtps_path_set_congested(this->path);
          _transit_state_to(this, STATE_OVERUSED);
          _disable_controlling(this);
          _disable_monitoring(this);
        break;
        case EVENT_DISTORTION:
          _transit_state_to(this, STATE_OVERUSED);
          _disable_monitoring(this);
        break;
        case EVENT_STEADY:
          subanalyser_reset_stability(this->analyser);
          _transit_state_to(this, STATE_STABLE);
          _disable_monitoring(this);
        break;
        case EVENT_PROBE:
          _setup_raise_monitoring(this);
          break;
        case EVENT_FI:
        default:
        break;
      }
    break;
    default:
      g_warning("The desired state not exists.");
      break;
  }
}


void _switch_stage_to(
    SubflowRateController *this,
    Stage target,
    gboolean execute)
{
  switch(target){
     case STAGE_KEEP:
       this->stage_fnc = _keep_stage;
     break;
     case STAGE_REDUCE:
       this->stage_fnc = _reduce_stage;
     break;
     case STAGE_BOUNCE:
       this->stage_fnc = _bounce_stage;
     break;
     case STAGE_MITIGATE:
       this->stage_fnc = _mitigate_stage;
     break;
     case STAGE_RAISE:
       this->stage_fnc = _raise_stage;
     break;
   }
  _mt0(this)->stage = target;
  if(execute){
      this->stage_fnc(this);
  }
}

gint32 subratectrler_get_target_bitrate(SubflowRateController *this)
{
  return this->target_bitrate;
}

gint32 subratectrler_get_monitoring_bitrate(SubflowRateController *this)
{
  return this->monitored_bitrate;
}

void _disable_controlling(SubflowRateController *this)
{
  GstClockTime interval;
  interval = _mt0(this)->time - _mt1(this)->time;
  this->disable_controlling = _now(this) +  MIN(2 * GST_SECOND, 2 * interval);
}

void _reset_monitoring(SubflowRateController *this)
{
  this->monitoring_interval = 0;
  this->monitoring_started = 0;
  this->monitored_bitrate = 0;
}

void _setup_bounce_monitoring(SubflowRateController *this)
{
  guint interval;
  gdouble plus_rate = 0, scl = 0;
  guint max_interval, min_interval;

  max_interval = MAX_MONITORING_INTERVAL;
  min_interval = MIN_BOUNCE_MONITORING_INTERVAL;

  scl =  (gdouble) this->min_rate;
  scl /= (gdouble)(_TR(this) - this->min_rate * .5);
  scl *= scl;
  scl = MIN(1., MAX(1./14., scl));
  plus_rate = _TR(this) * scl;
  plus_rate = MIN(this->bottleneck_point * .9, plus_rate);

  interval = _calculate_monitoring_interval(this, plus_rate);
  this->monitoring_interval = CONSTRAIN(min_interval, max_interval, interval);
  _set_monitoring_interval(this, this->monitoring_interval);
}

void _setup_raise_monitoring(SubflowRateController *this)
{
  if(this->bottleneck_point < _TR(this) * .9 || _TR(this) * 1.1 < this->bottleneck_point){
    this->monitoring_interval = 5;
  }else{
    this->monitoring_interval = 10;
  }
  _set_monitoring_interval(this, this->monitoring_interval);
  this->desired_bitrate  = _TR(this) + this->monitored_bitrate;
}
//
//void _setup_raise_monitoring(SubflowRateController *this)
//{
//  guint interval;
//  gdouble plus_rate = 0, scl = 0;
//  guint max_interval, min_interval;
//  max_interval = MAX_MONITORING_INTERVAL;
//  min_interval = MIN_PROBE_MONITORING_INTERVAL;
//
//  if(!this->bottleneck_point){
//    plus_rate = (gdouble)_TR(this) / (gdouble)MIN_PROBE_MONITORING_INTERVAL;
//    goto done;
//  }
//
//  scl = (gdouble)(_TR(this) - this->bottleneck_point);
//  scl /=  (gdouble) (this->bottleneck_point * .2);
//  scl *= scl;
//  scl = MIN(1., MAX(1./14., scl));
//  plus_rate = _TR(this) * scl;
//done:
//  interval = _calculate_monitoring_interval(this, plus_rate);
//  this->monitoring_interval = CONSTRAIN(min_interval, max_interval, interval);
//  _set_monitoring_interval(this, this->monitoring_interval);
//  this->desired_bitrate  = _min_br(this) + this->monitored_bitrate;
//}

void _set_monitoring_interval(SubflowRateController *this, guint interval)
{
  this->monitoring_started = _now(this);
  if(interval > 0)
    this->monitored_bitrate = (gdouble)_TR(this) / (gdouble)interval;
  else
    this->monitored_bitrate = 0;
  return;
}

guint _calculate_monitoring_interval(SubflowRateController *this, guint32 desired_bitrate)
{
  gdouble actual, target, rate;
  guint monitoring_interval = 0;
  if(desired_bitrate <= 0){
     goto exit;
   }
  actual = _TR(this);
  target = actual + (gdouble) desired_bitrate;
  rate = target / actual;

  if(rate > 1.51) monitoring_interval = 2;
  else if(rate > 1.331) monitoring_interval = 3;
  else if(rate > 1.251) monitoring_interval = 4;
  else if(rate > 1.21) monitoring_interval = 5;
  else if(rate > 1.161) monitoring_interval = 6;
  else if(rate > 1.141) monitoring_interval = 7;
  else if(rate > 1.121) monitoring_interval = 8;
  else if(rate > 1.111) monitoring_interval = 9;
  else if(rate > 1.101) monitoring_interval = 10;
  else if(rate > 1.091) monitoring_interval = 11;
  else if(rate > 1.081) monitoring_interval = 12;
  else if(rate > 1.071) monitoring_interval = 13;
  else  monitoring_interval = 14;

exit:
  return monitoring_interval;

}

void _change_target_bitrate(SubflowRateController *this, gint32 new_target)
{
  if(0 < this->min_rate){
    this->target_bitrate = MAX(this->min_rate, new_target);
  }
  if(0 < this->max_rate){
    this->target_bitrate = MIN(this->max_rate, new_target);
  }
  g_print("TARGET RATE: %d\n", new_target);
}

gboolean _is_near_to_bottleneck_point(SubflowRateController *this)
{
  gint32 cc_point;
  cc_point = this->bottleneck_point;
  if(!cc_point) return FALSE;
  if( _min_br(this) * .9 < cc_point && cc_point < _max_br(this) * 1.1) return TRUE;
  return FALSE;
}

gdouble _get_bottleneck_influence(SubflowRateController *this)
{
  gdouble result;
  gdouble b_point = this->bottleneck_point;
  gdouble br, br1, br2;
  if(!this->bottleneck_point) return 0.;
  if(_max_br(this) * 1.1 < this->bottleneck_point) return 0.;
  if(  this->target_bitrate < _min_br(this) * .9 ) return 0.;
  br1 = abs(this->bottleneck_point - _TR(this));
  br2 = abs(this->bottleneck_point - _SR(this));
  br = br1 < br2 ? _TR(this) : _SR(this);
  result = br / (2. * (b_point - br));
  result *= result;
//  result  *= 0. < result ? 2. : -.5;
  return CONSTRAIN(.01, 1., result);
}

void
_add_bottleneck_point(
    SubflowRateController *this, gint32 rate)
{
  this->congestion_detected = _now(this);
  this->bottleneck_point = rate;
  DISABLE_LINE _append_to_log(this, "sg");
}

void _append_to_log(SubflowRateController *this, const gchar * format, ...)
{
  FILE *file;
  va_list args;
  if(!this->log_enabled) return;
  file = fopen(this->log_filename, "a");
  va_start (args, format);
  vfprintf (file, format, args);
  va_end (args);
  fclose(file);

  DISABLE_LINE _add_bottleneck_point(NULL, 0);
}

void _log_measurement_update_state(SubflowRateController *this)
{
  FILE *file;
  if(!this->log_enabled) return;
  file = fopen(this->log_filename, "a");
  if(++this->logtick % 60 == 0)
    _log_abbrevations(this, file);
  fprintf(file,
//  g_print (

          "############ S%d | State: %-2d | Disable time %lu | Ctrled: %d #################\n"
          "rlost:      %-10d| rdiscard:%-10d| lost:    %-10d| discard: %-10d|\n"
          "pth_cong:   %-10d| pth_lssy:%-10d| dis_br:  %-10d| GP_t1:   %-10d|\n"
          "target_br:  %-10d| min_tbr: %-10d| max_tbr: %-10d| trend:   %-10.6f\n"
          "stage:      %-10d| near2cc: %-10d| exp_lst: %-10d| pevent:  %-10d|\n"
          "mon_br:     %-10d| mon_int: %-10d| btlnck:  %-10d| GP:      %-10d|\n"
          "RR:         %-10d| SR:      %-10d| disc_rat:%-10.3f| ci:      %-10.3f|\n"
          "l:          %-10d| rl:      %-10d| d:       %-10d| rd:      %-10d\n"
          "abs_max:    %-10d| abs_min: %-10d| event:      %-7d| off_add: %-10.3f\n"
          "keep:       %-10d| now:    %-10lu| cong:    %-10d|\n"
          "Analysis--------------> pierced: %-7d| distorted: %-7d| congested: %-7d|\n"
          "----------------------> tr_corr: %-7d| stable:  %-7d|\n"
          "############################ Seconds since setup: %lu ##########################################\n",
          this->id, _state(this),
          this->disable_controlling > 0 ? GST_TIME_AS_MSECONDS(this->disable_controlling - _now(this)) : 0,
          _mt0(this)->controlled,

          _rlost(this),_rdiscard(this),_lost(this),_discard(this),

          !mprtps_path_is_non_congested(this->path),
          !mprtps_path_is_non_lossy(this->path),
          this->desired_bitrate,
          _GP_t1(this),

          this->target_bitrate, this->min_target_point,
          this->max_target_point, _qtrend(this),

          _stage(this),
          _is_near_to_bottleneck_point(this),
          _mt0(this)->has_expected_lost, this->pending_event,

          this->monitored_bitrate, this->monitoring_interval,
          this->bottleneck_point, _GP(this),

          _RR(this), _mt0(this)->sender_bitrate,
          _mt0(this)->discard_rate, _get_bottleneck_influence(this),

          _mt0(this)->lost, _mt0(this)->recent_lost,
          _mt0(this)->discard,_mt0(this)->recent_discard,

          this->max_rate, this->min_rate, _event(this), _anres(this).off,

          this->keep,
          GST_TIME_AS_SECONDS(_now(this)),
          _anres(this).congested,

          _anres(this).pierced,
          _anres(this).distorted,
          _anres(this).congested,

          _anres(this).tr_correlated,
          _anres(this).stable,

         GST_TIME_AS_SECONDS(_now(this) - this->setup_time)

         );
  subanalyser_append_logfile(this->analyser, file);
  fclose(file);
}

void _log_abbrevations(SubflowRateController *this, FILE *file)
{
  fprintf(file,
    "############ Subflow %d abbrevations ##############################################################\n"
    "#  State:      The actual state (Overused (-1), Stable (0), Monitored (1))                        #\n"
    "#  Ctrled:     Indicate weather the state is controlled or not                                    #\n"
    "#  rlost:      recent losts                                                                       #\n"
    "#  rdiscard:   recent discards                                                                    #\n"
    "#  lost:       any losts                                                                          #\n"
    "#  pth_lssy:   Indicate weather the path is lossy                                                 #\n"
    "#  pth_cong:   Indicate weather the path is congested                                             #\n"
    "#  pth_slow:   Indicate weather the path is slow                                                  #\n"
    "###################################################################################################\n",
    this->id
  );
}

