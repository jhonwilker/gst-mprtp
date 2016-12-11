#!/bin/bash
programname=$0
LOGSDIR="temp"
TEMPDIR="temp_batch"

TEST="rmcat6"
OWD=50
JITTER=0

mkdir $TEMPDIR
rm $TEMPDIR/*

if [ -z "$1" ]
then
  #ALGORITHM="SCReAM"
  ALGORITHM="FRACTaL"
else
  ALGORITHM=$1
fi

if [ -z "$2" ]
then
  OWD=300
else
  OWD=$2
fi


SNDFILE="$TEMPDIR/snd.sh"
echo "./scripts/runs/snd/$TEST.sh $ALGORITHM" > $SNDFILE
chmod 777 $SNDFILE

RCVFILE="$TEMPDIR/rcv.sh"
echo "./scripts/runs/rcv/$TEST.sh $ALGORITHM" > $RCVFILE
chmod 777 $RCVFILE

#For the initial OWD to be right
./scripts/runs/$TEST.sh $OWD &
sleep 5
sudo pkill ntrt

COUNTER=0
if [ -z "$3" ]
then
  END=11
else
  END=$3
fi

while [  $COUNTER -lt $END ]; do
    echo "The counter is $COUNTER"
    
	sudo ip netns exec ns_rcv $RCVFILE &
	sleep 2
	sudo ip netns exec ns_snd $SNDFILE &
	sleep 2
	./scripts/runs/$TEST.sh $OWD &
	sleep 10
	
	INCREASE=1  
	
	#Flow 1
	for FILE in snd_packetlogs.csv rcv_packetlogs.csv snd_statlogs.csv rcv_statlogs.csv
	do
   		if [ ! -f $LOGSDIR"/"$FILE ]; then
    		INCREASE=0
    		echo $FILE" not found"
		fi
	done
	
	if [ $INCREASE -eq 0 ]
	then
	  sudo pkill snd_pipeline
	  sudo pkill rcv_pipeline
	  sudo pkill ntrt
	  sudo pkill iperf
	  continue
	fi
	
	
	sleep 50
	
	#Validation Part 2
	for FILE in snd_packetlogs.csv rcv_packetlogs.csv  
	do
		minimumsize=90000
		actualsize=$(wc -c <"$LOGSDIR/$FILE")
		if [ ! $actualsize -ge $minimumsize ]; then
		    echo "-----$FILE SIZE IS UNDER $minimumsize BYTES-----"
		    INCREASE=0
    		echo $FILE" not found"
		fi
	done
	
	if [ $INCREASE -eq 0 ]
	then
	  sudo pkill snd_pipeline
	  sudo pkill rcv_pipeline
	  sudo pkill ntrt
	  sudo pkill iperf
	  continue
	fi
	
	sleep 150
	
	sudo pkill snd_pipeline
	sudo pkill rcv_pipeline
	sudo pkill ntrt
	sudo pkill iperf
	
	alg=${ALGORITHM,,}

	TARGET="$TEMPDIR/"$alg"_"$COUNTER"_"$OWD"ms_"$JITTER"ms"
	mkdir $TARGET
	cp temp/* $TARGET
	
	
    let COUNTER=COUNTER+$INCREASE
done

