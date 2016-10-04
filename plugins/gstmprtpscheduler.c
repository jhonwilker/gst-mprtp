/* GStreamer
 * Copyright (C) 2015 FIXME <fixme@example.com>
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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:element-gstmprtpscheduler
 *
 * The mprtpscheduler element does FIXME stuff.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v fakesrc ! mprtpscheduler ! FIXME ! fakesink
 * ]|
 * FIXME Describe what the pipeline does.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gst/gst.h>
#include "gstmprtpscheduler.h"
#include "streamsplitter.h"
#include "gstmprtcpbuffer.h"
#include "sndctrler.h"
#include "rtppackets.h"
#include <sys/timex.h>


GST_DEBUG_CATEGORY_STATIC (gst_mprtpscheduler_debug_category);
#define GST_CAT_DEFAULT gst_mprtpscheduler_debug_category

#define PACKET_IS_RTP_OR_RTCP(b) (b > 0x7f && b < 0xc0)
#define PACKET_IS_RTCP(b) (b > 192 && b < 223)
#define THIS_LOCK(this) (g_mutex_lock(&this->mutex))
#define THIS_UNLOCK(this) (g_mutex_unlock(&this->mutex))

#define _now(this) gst_clock_get_time (this->sysclock)

static void gst_mprtpscheduler_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_mprtpscheduler_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);
static void gst_mprtpscheduler_dispose (GObject * object);
static void gst_mprtpscheduler_finalize (GObject * object);

static GstStateChangeReturn
gst_mprtpscheduler_change_state (GstElement * element,
    GstStateChange transition);
static gboolean gst_mprtpscheduler_query (GstElement * element,
    GstQuery * query);
static GstFlowReturn gst_mprtpscheduler_rtp_sink_chain (GstPad * pad,
    GstObject * parent, GstBuffer * buffer);
static GstFlowReturn gst_mprtpscheduler_rtp_sink_chainlist (GstPad * pad,
    GstObject * parent, GstBufferList * list);

static GstFlowReturn gst_mprtpscheduler_mprtcp_rr_sink_chain (GstPad * pad,
    GstObject * parent, GstBuffer * outbuf);

static gboolean gst_mprtpscheduler_src_query (GstPad * srckpad, GstObject * parent,
    GstQuery * query);

static gboolean gst_mprtpscheduler_mprtp_src_event (GstPad * pad,
    GstObject * parent, GstEvent * event);
static gboolean gst_mprtpscheduler_sink_eventfunc (GstPad * srckpad, GstObject * parent,
                                                   GstEvent * event);

static void _on_monitoring_request(GstMprtpscheduler * this, SndSubflow* subflow);
static void _on_monitoring_response(GstMprtpscheduler * this, FECEncoderResponse *response);
static gboolean _mprtpscheduler_send_packet (GstMprtpscheduler * this, RTPPacket *packet);
static void mprtpscheduler_approval_process(gpointer udata);
static void mprtpscheduler_emitter_process(gpointer udata);


static guint _subflows_utilization;

enum
{
  PROP_0,
  PROP_MPRTP_EXT_HEADER_ID,
  PROP_ABS_TIME_EXT_HEADER_ID,
  PROP_FEC_PAYLOAD_TYPE,
  PROP_MPATH_KEYFRAME_FILTERING,
  PROP_PACKET_OBSOLATION_TRESHOLD,
  PROP_JOIN_SUBFLOW,
  PROP_DETACH_SUBFLOW,
  PROP_SET_SUBFLOW_NON_CONGESTED,
  PROP_SET_SUBFLOW_CONGESTED,
  PROP_SETUP_CONTROLLING_MODE,
  PROP_SET_SENDING_TARGET,
  PROP_SETUP_RTCP_INTERVAL_TYPE,
  PROP_SETUP_REPORT_TIMEOUT,
  PROP_FEC_INTERVAL,
  PROP_LOG_ENABLED,
  PROP_LOG_PATH,
};

/* signals and args */
enum
{
  SIGNAL_SYSTEM_STATE,
  LAST_SIGNAL
};

/* pad templates */
static GstStaticPadTemplate gst_mprtpscheduler_rtp_sink_template =
GST_STATIC_PAD_TEMPLATE ("rtp_sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-rtp")
    );


static GstStaticPadTemplate gst_mprtpscheduler_mprtp_src_template =
GST_STATIC_PAD_TEMPLATE ("mprtp_src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-rtp")
    );


static GstStaticPadTemplate gst_mprtpscheduler_mprtcp_rr_sink_template =
GST_STATIC_PAD_TEMPLATE ("mprtcp_rr_sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
//    GST_STATIC_CAPS ("application/x-rtcp")
    GST_STATIC_CAPS_ANY);


static GstStaticPadTemplate gst_mprtpscheduler_mprtcp_sr_src_template =
GST_STATIC_PAD_TEMPLATE ("mprtcp_sr_src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
//    GST_STATIC_CAPS ("application/x-rtcp")
    GST_STATIC_CAPS_ANY);


/* class initialization */
G_DEFINE_TYPE_WITH_CODE (GstMprtpscheduler, gst_mprtpscheduler,
    GST_TYPE_ELEMENT,
    GST_DEBUG_CATEGORY_INIT (gst_mprtpscheduler_debug_category,
        "mprtpscheduler", 0, "debug category for mprtpscheduler element"));

#define GST_MPRTPSCHEDULER_GET_PRIVATE(obj)  \
    (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_MPRTPSCHEDULER, GstMprtpschedulerPrivate))

struct _GstMprtpschedulerPrivate
{

};

static void
gst_mprtpscheduler_class_init (GstMprtpschedulerClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_mprtpscheduler_rtp_sink_template));

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_mprtpscheduler_mprtp_src_template));

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_mprtpscheduler_mprtcp_sr_src_template));

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get
      (&gst_mprtpscheduler_mprtcp_rr_sink_template));


  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "MPRTP Scheduler", "Generic", "MPRTP scheduler FIXME LONG DESC",
      "Balázs Kreith <balazs.kreith@gmail.com>");

  gobject_class->set_property = gst_mprtpscheduler_set_property;
  gobject_class->get_property = gst_mprtpscheduler_get_property;
  gobject_class->dispose = gst_mprtpscheduler_dispose;
  gobject_class->finalize = gst_mprtpscheduler_finalize;
  element_class->change_state =
      GST_DEBUG_FUNCPTR (gst_mprtpscheduler_change_state);
  element_class->query = GST_DEBUG_FUNCPTR (gst_mprtpscheduler_query);


  g_object_class_install_property (gobject_class, PROP_MPRTP_EXT_HEADER_ID,
      g_param_spec_uint ("mprtp-ext-header-id",
          "Set or get the id for the Multipath RTP extension",
          "Sets or gets the id for the extension header the MpRTP based on. The default is 3",
          0, 15, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_ABS_TIME_EXT_HEADER_ID,
      g_param_spec_uint ("abs-time-ext-header-id",
          "Set or get the id for the absolute time RTP extension",
          "Sets or gets the id for the extension header the absolute time based on. The default is 8",
          0, 15, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_FEC_PAYLOAD_TYPE,
      g_param_spec_uint ("fec-payload-type",
          "Set or get the payload type of FEC packets",
          "Set or get the payload type of FEC packets. The default is 126",
          0, 127, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_MPATH_KEYFRAME_FILTERING,
      g_param_spec_uint ("mpath-keyframe-filtering",
          "Set or get the keyframe filtering for multiple path",
          "Set or get the keyframe filtering for multiple path. 0 - no keyframe filtering, 1 - vp8 enc/dec filtering",
          0, 255, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_PACKET_OBSOLATION_TRESHOLD,
      g_param_spec_uint ("obsolation-treshold",
          "Set the obsolation treshold at the packet sender queue.",
          "Set the obsolation treshold at the packet sender queue.",
          0, 10000, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_JOIN_SUBFLOW,
      g_param_spec_uint ("join-subflow", "the subflow id requested to join",
          "Join a subflow with a given id.", 0,
          MPRTP_PLUGIN_MAX_SUBFLOW_NUM, 0, G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_DETACH_SUBFLOW,
      g_param_spec_uint ("detach-subflow", "the subflow id requested to detach",
          "Detach a subflow with a given id.", 0,
          MPRTP_PLUGIN_MAX_SUBFLOW_NUM, 0, G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_SET_SUBFLOW_CONGESTED,
      g_param_spec_uint ("congested-subflow", "set the subflow congested",
          "Set the subflow congested", 0,
          MPRTP_PLUGIN_MAX_SUBFLOW_NUM, 0, G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
      PROP_SET_SUBFLOW_NON_CONGESTED,
      g_param_spec_uint ("non-congested-subflow",
          "set the subflow non-congested", "Set the subflow non-congested", 0,
          MPRTP_PLUGIN_MAX_SUBFLOW_NUM, 0, G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_SET_SENDING_TARGET,
      g_param_spec_uint ("setup-sending-target",
          "set the sending target of the subflow",
          "A 32bit unsigned integer for setup a target. The first 8 bit identifies the subflow, the latter the target",
          0, 4294967295, 0, G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_SETUP_RTCP_INTERVAL_TYPE,
     g_param_spec_uint ("setup-rtcp-interval-type",
                        "RTCP interval types: 0 - regular, 1 - early, 2 - immediate feedback",
                        "A 32bit unsigned integer for setup a target. The first 8 bit identifies the subflow, the latter the mode. "
                        "RTCP interval types: 0 - regular, 1 - early, 2 - immediate feedback",
                        0,
                        4294967295, 2, G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_SETUP_REPORT_TIMEOUT,
      g_param_spec_uint ("setup-report-timeout",
          "setup a timeout value for incoming reports on subflows",
          "A 32bit unsigned integer for setup a target. The first 8 bit identifies the subflow, the latter the timeout in ms",
          0, 4294967295, 0, G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_SETUP_CONTROLLING_MODE,
      g_param_spec_uint ("setup-controlling-mode",
          "set the controlling mode to the subflow",
          "A 32bit unsigned integer for setup a target. The first 8 bit identifies the subflow, the latter the mode. "
          "0 - no sending rate controller, 1 - no controlling, but sending SRs, 2 - FBRA with MARC",
          0, 4294967295, 0, G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_FEC_INTERVAL,
      g_param_spec_uint ("fec-interval",
          "Set a stable FEC interval applied on the media stream",
          "The property value other than 0 request a FEC protection after a specified packet was sent. "
          "The newly created FEC packet is going to be sent on the path actually selected if the plugin uses multipath.",
          0, 15, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_LOG_ENABLED,
      g_param_spec_boolean ("logging",
          "Indicate weather a log for subflow is enabled or not",
          "Indicate weather a log for subflow is enabled or not",
          FALSE, G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));


  g_object_class_install_property (gobject_class, PROP_LOG_PATH,
        g_param_spec_string ("logs-path",
            "Determines the path for logfiles",
            "Determines the path for logfiles",
            "NULL", G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));

  _subflows_utilization =
      g_signal_new ("mprtp-subflows-utilization", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstMprtpschedulerClass, mprtp_media_rate_utilization),
      NULL, NULL, g_cclosure_marshal_generic, G_TYPE_NONE, 1,
      G_TYPE_POINTER);


}

static void
gst_mprtpscheduler_init (GstMprtpscheduler * this)
{
//  GstMprtpschedulerPrivate *priv;
//  priv = this->priv = GST_MPRTPSCHEDULER_GET_PRIVATE (this);

  init_mprtp_logger();

  this->rtp_sinkpad =
      gst_pad_new_from_static_template (&gst_mprtpscheduler_rtp_sink_template,
      "rtp_sink");

  gst_pad_set_chain_function (this->rtp_sinkpad,
      GST_DEBUG_FUNCPTR (gst_mprtpscheduler_rtp_sink_chain));
  gst_pad_set_chain_list_function (this->rtp_sinkpad,
      GST_DEBUG_FUNCPTR (gst_mprtpscheduler_rtp_sink_chainlist));

  GST_PAD_SET_PROXY_CAPS (this->rtp_sinkpad);
  GST_PAD_SET_PROXY_ALLOCATION (this->rtp_sinkpad);


  gst_element_add_pad (GST_ELEMENT (this), this->rtp_sinkpad);

  this->mprtcp_rr_sinkpad =
      gst_pad_new_from_static_template
      (&gst_mprtpscheduler_mprtcp_rr_sink_template, "mprtcp_rr_sink");

  gst_pad_set_chain_function (this->mprtcp_rr_sinkpad,
      GST_DEBUG_FUNCPTR (gst_mprtpscheduler_mprtcp_rr_sink_chain));

  gst_pad_set_event_function (this->rtp_sinkpad,
      GST_DEBUG_FUNCPTR (gst_mprtpscheduler_sink_eventfunc));

  gst_element_add_pad (GST_ELEMENT (this), this->mprtcp_rr_sinkpad);

  this->mprtcp_sr_srcpad =
      gst_pad_new_from_static_template
      (&gst_mprtpscheduler_mprtcp_sr_src_template, "mprtcp_sr_src");

  gst_element_add_pad (GST_ELEMENT (this), this->mprtcp_sr_srcpad);

  this->mprtp_srcpad =
      gst_pad_new_from_static_template (&gst_mprtpscheduler_mprtp_src_template,
      "mprtp_src");

  gst_pad_set_event_function (this->mprtp_srcpad,
      GST_DEBUG_FUNCPTR (gst_mprtpscheduler_mprtp_src_event));
  gst_pad_use_fixed_caps (this->mprtp_srcpad);
  GST_PAD_SET_PROXY_CAPS (this->mprtp_srcpad);
  GST_PAD_SET_PROXY_ALLOCATION (this->mprtp_srcpad);

  gst_pad_set_query_function(this->mprtp_srcpad,
    GST_DEBUG_FUNCPTR(gst_mprtpscheduler_src_query));

  gst_element_add_pad (GST_ELEMENT (this), this->mprtp_srcpad);


  this->sysclock = gst_system_clock_obtain ();
  this->thread = gst_task_new (mprtpscheduler_emitter_process, this, NULL);
  g_mutex_init (&this->mutex);

  this->fec_payload_type = FEC_PAYLOAD_DEFAULT_ID;
  this->splitter = make_stream_splitter(this->subflows);

  this->packetforwarder  = make_packetforwarder(this->mprtp_srcpad, this->mprtcp_sr_srcpad);

  this->monitoring    = make_mediator();

  this->subflows      = make_sndsubflows(this->monitoring);
  this->rtppackets    = make_rtppackets();
  this->rtp_in        = g_async_queue_new();
  this->fec_responses = g_async_queue_new();
  this->emitterq      = g_async_queue_new();


  this->fec_encoder   = make_fecencoder(this->monitoring);
  this->sndtracker    = make_sndtracker(this->subflows);

  this->controller = make_sndctrler(
      this->sndtracker,
      this->subflows,
      this->mprtcpq,
      this->emitterq);

  this->mprtpq        = g_async_queue_ref(this->packetforwarder->mprtpq);
  this->mprtcpq       = g_async_queue_ref(this->packetforwarder->mprtcpq);

  fecencoder_set_payload_type(this->fec_encoder, this->fec_payload_type);
  sndsubflows_add_on_target_bitrate_changed_cb(this->subflows,
      (NotifierFunc) stream_splitter_on_target_bitrate_changed, this->splitter);

  mediator_set_request_handler(this->monitoring,
        (NotifierFunc) _on_monitoring_request, this);

  mediator_set_response_handler(this->monitoring,
        (NotifierFunc) _on_monitoring_response, this);
}


void
gst_mprtpscheduler_dispose (GObject * object)
{
  GstMprtpscheduler *mprtpscheduler = GST_MPRTPSCHEDULER (object);

  GST_DEBUG_OBJECT (mprtpscheduler, "dispose");

  /* clean up as possible.  may be called multiple times */

  G_OBJECT_CLASS (gst_mprtpscheduler_parent_class)->dispose (object);
}

void
gst_mprtpscheduler_finalize (GObject * object)
{
  GstMprtpscheduler *this = GST_MPRTPSCHEDULER (object);

  GST_DEBUG_OBJECT (this, "finalize");

  /* clean up object here */
  gst_task_join (this->thread);
  gst_object_unref (this->thread);

  g_object_unref (this->sysclock);
  g_object_unref (this->subflows);
  g_object_unref (this->rtppackets);
  g_object_unref (this->splitter);
  g_object_unref (this->fec_encoder);

  g_async_queue_unref(this->mprtpq);
  g_async_queue_unref(this->mprtcpq);
  g_async_queue_unref(this->rtp_in);
  g_async_queue_unref(this->fec_responses);
  g_async_queue_unref(this->emitterq);

  G_OBJECT_CLASS (gst_mprtpscheduler_parent_class)->finalize (object);
}


typedef struct _SubflowSpecProp{
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
  guint32  value : 24;
  guint32  id     : 8;
#elif G_BYTE_ORDER == G_BIG_ENDIAN
  guint32  id     : 8;
  guint32  value : 24;
#else
#error "G_BYTE_ORDER should be big or little endian."
#endif
}SubflowSpecProp;

void
gst_mprtpscheduler_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstMprtpscheduler *this = GST_MPRTPSCHEDULER (object);
  guint guint_value;
  gboolean gboolean_value;
  SubflowSpecProp *subflow_prop;

  subflow_prop = (SubflowSpecProp*) &guint_value;
  GST_DEBUG_OBJECT (this, "set_property");

  THIS_LOCK(this);
  switch (property_id) {
    case PROP_MPRTP_EXT_HEADER_ID:
      rtppackets_set_mprtp_ext_header_id(this->rtppackets, (guint8) g_value_get_uint (value));
      break;
    case PROP_ABS_TIME_EXT_HEADER_ID:
      rtppackets_set_abs_time_ext_header_id(this->rtppackets, (guint8) g_value_get_uint (value));
      break;
    case PROP_FEC_PAYLOAD_TYPE:
      this->fec_payload_type = (guint8) g_value_get_uint (value);
      fecencoder_set_payload_type(this->fec_encoder, this->fec_payload_type);
      break;
    case PROP_JOIN_SUBFLOW:
      sndsubflows_join(this->subflows, g_value_get_uint (value));
      break;
    case PROP_MPATH_KEYFRAME_FILTERING:
      g_warning("path keyframe filtering is not implemented yet");
      break;
    case PROP_PACKET_OBSOLATION_TRESHOLD:
      this->obsolation_treshold = g_value_get_uint(value) * GST_MSECOND;
      break;
    case PROP_DETACH_SUBFLOW:
      sndsubflows_detach(this->subflows, g_value_get_uint (value));
      break;
    case PROP_SET_SUBFLOW_CONGESTED:
      guint_value = g_value_get_uint (value);
      sndsubflows_set_path_congested(this->subflows, guint_value, TRUE);
      break;
    case PROP_SET_SUBFLOW_NON_CONGESTED:
      guint_value = g_value_get_uint (value);
      sndsubflows_set_path_congested(this->subflows, guint_value, FALSE);
      break;
    case PROP_FEC_INTERVAL:
      this->fec_interval = g_value_get_uint (value);
      break;
    case PROP_SET_SENDING_TARGET:
      guint_value = g_value_get_uint (value);
      sndsubflows_set_target_bitrate(this->subflows, subflow_prop->id, subflow_prop->value);
      break;
    case PROP_SETUP_RTCP_INTERVAL_TYPE:
      sndsubflows_set_rtcp_interval_type(this->subflows, subflow_prop->id, subflow_prop->value);
      break;
    case PROP_SETUP_CONTROLLING_MODE:
      sndsubflows_set_congestion_controlling_type(this->subflows, subflow_prop->id, subflow_prop->value);
      break;
    case PROP_SETUP_REPORT_TIMEOUT:
      guint_value = g_value_get_uint (value);
      sndctrler_setup_report_timeout(this->controller,  subflow_prop->id, subflow_prop->value * GST_MSECOND);
      break;
    case PROP_LOG_ENABLED:
      gboolean_value = g_value_get_boolean (value);
      mprtp_logger_set_state(gboolean_value);
      break;
    case PROP_LOG_PATH:
      mprtp_logger_set_target_directory(g_value_get_string(value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
  THIS_UNLOCK(this);
}


void
gst_mprtpscheduler_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstMprtpscheduler *this = GST_MPRTPSCHEDULER (object);

  GST_DEBUG_OBJECT (this, "get_property");
  THIS_LOCK(this);
  switch (property_id) {
    case PROP_MPRTP_EXT_HEADER_ID:
      g_value_set_uint (value, (guint) rtppackets_get_mprtp_ext_header_id(this->rtppackets));
      break;
    case PROP_ABS_TIME_EXT_HEADER_ID:
      g_value_set_uint (value, (guint) rtppackets_get_abs_time_ext_header_id(this->rtppackets));
      break;
    case PROP_MPATH_KEYFRAME_FILTERING:
      g_warning("path keyframe filtering is not implemented yet");
      break;
    case PROP_FEC_PAYLOAD_TYPE:
      g_value_set_uint (value, (guint) this->fec_payload_type);
      break;
    case PROP_PACKET_OBSOLATION_TRESHOLD:
      g_value_set_uint(value, this->obsolation_treshold / GST_MSECOND);
      break;
    case PROP_FEC_INTERVAL:
      g_value_set_uint (value, (guint) this->fec_interval);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
  THIS_UNLOCK(this);

}


gboolean
gst_mprtpscheduler_mprtp_src_event (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  GstMprtpscheduler *this;
  gboolean result;

  this = GST_MPRTPSCHEDULER (parent);
  THIS_LOCK(this);
  switch (GST_EVENT_TYPE (event)) {
    default:
      result = gst_pad_push_event (this->rtp_sinkpad, event);
//      result = gst_pad_event_default (pad, parent, event);
  }
  THIS_UNLOCK(this);
  return result;
}


static GstStateChangeReturn
gst_mprtpscheduler_change_state (GstElement * element,
    GstStateChange transition)
{
  GstStateChangeReturn ret;
  GstMprtpscheduler * this;

  this = GST_MPRTPSCHEDULER (element);
  g_return_val_if_fail (GST_IS_MPRTPSCHEDULER (element),
      GST_STATE_CHANGE_FAILURE);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      gst_pad_start_task(this->mprtp_srcpad, (GstTaskFunction)mprtpscheduler_approval_process,
         this, NULL);
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
       gst_task_set_lock (this->thread, &this->thread_mutex);
       gst_task_start (this->thread); //Fixme elliminate if we doesn't want this.
       break;
     default:
       break;
   }

   ret =
       GST_ELEMENT_CLASS (gst_mprtpscheduler_parent_class)->change_state
       (element, transition);

   switch (transition) {
     case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
       gst_task_stop (this->thread);
       break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      break;
    default:
      break;
  }

  return ret;
}


static gboolean
gst_mprtpscheduler_query (GstElement * element, GstQuery * query)
{
  GstMprtpscheduler *this = GST_MPRTPSCHEDULER (element);
  gboolean ret = TRUE;
  GstStructure *s = NULL;

  GST_DEBUG_OBJECT (this, "query");
  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CUSTOM:
      THIS_LOCK(this);
      s = gst_query_writable_structure (query);
      if (!gst_structure_has_name (s,
              GST_MPRTCP_SCHEDULER_SENT_BYTES_STRUCTURE_NAME)) {
        ret =
            GST_ELEMENT_CLASS (gst_mprtpscheduler_parent_class)->query (element,
            query);
        break;
      }
      gst_structure_set (s,
          GST_MPRTCP_SCHEDULER_SENT_OCTET_SUM_FIELD,
          G_TYPE_UINT, this->rtcp_sent_octet_sum, NULL);
      THIS_UNLOCK(this);
      ret = TRUE;
      break;
    default:
      ret =
          GST_ELEMENT_CLASS (gst_mprtpscheduler_parent_class)->query (element,
          query);
      break;
  }

  return ret;
}



static GstFlowReturn
gst_mprtpscheduler_rtp_sink_chain (GstPad * pad, GstObject * parent,
    GstBuffer * buffer)
{
  GstMprtpscheduler *this;
  GstFlowReturn result;
  guint8 first_byte;
  guint8 second_byte;
//  gboolean suggest_to_skip = FALSE;
//  GstBuffer *outbuf;
  result = GST_FLOW_OK;

  this = GST_MPRTPSCHEDULER (parent);

//  g_print("Sent: %lu\n", GST_TIME_AS_MSECONDS(gst_clock_get_time(this->sysclock)-prev));
//  prev = gst_clock_get_time(this->sysclock);
  if (gst_buffer_extract (buffer, 0, &first_byte, 1) != 1 ||
      gst_buffer_extract (buffer, 1, &second_byte, 1) != 1) {
    GST_WARNING_OBJECT (this, "could not extract first byte from buffer");
    gst_buffer_unref (buffer);
    goto done;
  }

  if (!PACKET_IS_RTP_OR_RTCP (first_byte)) {
    GST_DEBUG_OBJECT (this, "Not RTP Packet arrived at rtp_sink");
    g_async_queue_push(this->mprtpq, buffer);
    goto done;
  }
  if(PACKET_IS_RTCP(second_byte)){
    GST_DEBUG_OBJECT (this, "RTCP Packet arrived on rtp sink");
    g_async_queue_push(this->mprtpq, buffer);
    goto done;
  }

  g_async_queue_push(this->rtp_in, gst_buffer_ref(buffer));
done:
  result = GST_FLOW_OK;
  return result;
}


static GstFlowReturn
gst_mprtpscheduler_rtp_sink_chainlist (GstPad * pad, GstObject * parent,
    GstBufferList * list)
{
  GstBuffer *buffer;
  gint i, len;
  GstFlowReturn result;

  result = GST_FLOW_OK;

  /* chain each buffer in list individually */
  len = gst_buffer_list_length (list);

  if (len == 0)
    goto done;

  for (i = 0; i < len; i++) {
    buffer = gst_buffer_list_get (list, i);

    result = gst_mprtpscheduler_rtp_sink_chain (pad, parent, buffer);
    if (result != GST_FLOW_OK)
      break;
  }

done:
  return result;
}


static GstFlowReturn
gst_mprtpscheduler_mprtcp_rr_sink_chain (GstPad * pad, GstObject * parent,
    GstBuffer * buf)
{
  GstMprtpscheduler *this;
  GstFlowReturn result;

  this = GST_MPRTPSCHEDULER (parent);
  GST_DEBUG_OBJECT (this, "RTCP/MPRTCP sink");
  THIS_LOCK(this);

  sndctrler_receive_mprtcp(this->controller, buf);
  result = GST_FLOW_OK;

  THIS_UNLOCK(this);
  return result;

}


static gboolean
gst_mprtpscheduler_sink_eventfunc (GstPad * srckpad, GstObject * parent,
                                   GstEvent * event)
{
  GstMprtpscheduler * this;
  gboolean result = TRUE, forward = TRUE;

  this = GST_MPRTPSCHEDULER(parent);
  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_START:
      break;
    case GST_EVENT_FLUSH_STOP:
      /* we need new segment info after the flush. */
      gst_segment_init (&this->segment, GST_FORMAT_UNDEFINED);
      this->position_out = GST_CLOCK_TIME_NONE;
      break;
    case GST_EVENT_EOS:
      break;
    case GST_EVENT_TAG:
      break;
    case GST_EVENT_SEGMENT:
    {
      gst_event_copy_segment (event, &this->segment);
      GST_DEBUG_OBJECT (this, "received SEGMENT %" GST_SEGMENT_FORMAT,
          &this->segment);
      break;
    }
    default:
      break;
  }

  if (result && forward)
    result = gst_pad_push_event (this->mprtp_srcpad, event);
  else
    gst_event_unref (event);

  return result;
}


static gboolean
gst_mprtpscheduler_src_query (GstPad * srckpad, GstObject * parent,
    GstQuery * query)
{
  GstMprtpscheduler *this = GST_MPRTPSCHEDULER (parent);
  gboolean result;

  GST_DEBUG_OBJECT (this, "query");
  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_LATENCY:
    {
      gboolean live;
      GstClockTime min, max;
      GstPad *peer;
      peer = gst_pad_get_peer (this->rtp_sinkpad);
      if ((result = gst_pad_query (peer, query))) {
          gst_query_parse_latency (query, &live, &min, &max);
          min= GST_MSECOND;
          max = -1;
          gst_query_set_latency (query, live, min, max);
      }
      gst_object_unref (peer);
    }
    break;
    default:
      result = gst_pad_query_default(srckpad, parent, query);
      break;
  }

  return result;
}

void _on_monitoring_request(GstMprtpscheduler * this, SndSubflow* subflow)
{
  fecencoder_request_fec(this->fec_encoder, subflow->id);
  this->monitoring_request = TRUE;
}

void _on_monitoring_response(GstMprtpscheduler * this, FECEncoderResponse *response)
{
  g_async_queue_push(this->fec_responses, response);
}

gboolean
_mprtpscheduler_send_packet (GstMprtpscheduler * this, RTPPacket *packet)
{
  gboolean result = FALSE;
  SndSubflow *subflow;
  GstClockTime now = _now(this);

  subflow = stream_splitter_approve_packet(this->splitter, packet, now);
  if(!subflow){
    goto done;
  }

  if(0 < this->fec_interval && (++this->sent_packets % this->fec_interval) == 0){
    sndsubflow_request_monitoring(subflow);
  }
  //TODO: add this line before entering the buffer to the thread
  fecencoder_add_rtpbuffer(this->fec_encoder, packet->buffer);

  result = TRUE;
  rtppacket_setup_mprtp(packet, subflow);

  rtppacket_setup_abs_time_extension(packet);
  sndtracker_packet_sent(this->sndtracker, packet);
  packetforwarder_add_rtppad_buffer(this->packetforwarder, gst_buffer_ref(packet->buffer));
  rtppackets_packet_forwarded(this->rtppackets, packet);


  if (!this->riport_flow_signal_sent) {
    this->riport_flow_signal_sent = TRUE;
    sndctrler_report_can_flow(this->controller);
  }
done:
  return result;
}


void
mprtpscheduler_approval_process (gpointer udata)
{
  GstMprtpscheduler *this;
  GstBuffer* buffer;
  RTPPacket *packet;

  this = (GstMprtpscheduler *) udata;
  THIS_LOCK(this);

  sndctrler_time_update(this->controller);

  if(this->monitoring_request){
    FECEncoderResponse *response;
    response = g_async_queue_pop(this->fec_responses);
    sndtracker_add_fec_response(this->sndtracker, response);
    packetforwarder_add_rtppad_buffer(this->packetforwarder, response->fecbuffer);
    fecencoder_unref_response(response);
    this->monitoring_request = FALSE;
  }else{
    //TODO: refresh sndtracker fec indow
  }

  buffer = (GstBuffer *)g_async_queue_timeout_pop(this->rtp_in, 100);
  if(!buffer){
    sndtracker_refresh(this->sndtracker);
    goto done;
  }
  packet = rtppackets_retrieve_packet_for_sending(this->rtppackets, buffer);
  gst_buffer_unref(buffer);

  //Obsolete packets stayed in the q for a while
  if(0 < this->obsolation_treshold && packet->created < _now(this) - this->obsolation_treshold){
    rtppackets_packet_forwarded(this->rtppackets, packet);
    goto done;
  }

  if(!_mprtpscheduler_send_packet(this, packet)){
    g_async_queue_push_front(this->rtp_in, gst_buffer_ref(buffer));
    goto done;
  }
done:
  THIS_UNLOCK(this);
  return;
}


void
mprtpscheduler_emitter_process (gpointer udata)
{
  GstMprtpscheduler *this;
  MPRTPPluginSignalData *msg;

  this = (GstMprtpscheduler *) udata;
  msg  = (MPRTPPluginSignalData *)g_async_queue_timeout_pop(this->emitterq, 1000);
  if(!msg){
    goto done;
  }
  //at least 10 packet or 100ms must be elapsed between two signal
  g_signal_emit (this, _subflows_utilization, 0 /* details */, msg);
  g_slice_free(MPRTPPluginSignalData, msg);

done:
  return;
}

