#!/bin/bash
programname=$0

TEMPDIR="temp"
SCRIPTSDIR="scripts"
ACTDIR=$SCRIPTSDIR"/runs/snd"

SCREAM="SCReAM"
FRACTAL="FRACTaL"
#CC=$SCREAM
CC=$FRACTAL


#setup defaults
DURATION=150

SCRIPTFILE=$TEMPDIR"/sender.sh"

echo -n "./snd_pipeline "                                  > $SCRIPTFILE
echo -n "--source=FILE:foreman_cif.yuv:1:352:288:2:25/1 " >> $SCRIPTFILE
echo -n "--sourcesink=FILE:produced.yuv "                 >> $SCRIPTFILE

echo -n "--codec=VP8 "                                    >> $SCRIPTFILE
echo -n "--stat=100:1000:1:triggered_stat "               >> $SCRIPTFILE
echo -n "--statlogsink=FILE:temp/snd_statlogs.csv "       >> $SCRIPTFILE
echo -n "--packetlogsink=FILE:temp/snd_packetlogs.csv "   >> $SCRIPTFILE

echo $CC" is used as congestion control for sender 1"
if [ $CC = $SCREAM ] 
then
	echo -n "--sender=RTP:10.0.0.6:5000 "                     >> $SCRIPTFILE
	echo -n "--scheduler=SCREAM:RTP:5001 "                    >> $SCRIPTFILE
elif [ $CC = $FRACTAL ] 
then 
	echo -n "--sender=MPRTP:1:1:10.0.0.6:5000 "                >> $SCRIPTFILE
	echo -n "--scheduler=MPRTPFRACTAL:MPRTP:1:1:5001 "         >> $SCRIPTFILE
fi

    
chmod 777 $SCRIPTFILE

cleanup()
{
  pkill iperf
  pkill snd_pipeline
}
 
control_c()
# run if user hits control-c
{
  echo -en "\n*** Program is terminated ***\n"
  cleanup
  exit $?
}

trap control_c SIGINT
#Lets Rock
iperf -c 10.0.0.6 -p 12345 -t 120 &
sleep 5
./$SCRIPTFILE & 
sleep 150

cleanup
