# gst-mprtp

**What is it?**
  
Multipath RTP (MPRTP) is a header extension for RTP. 
It extends RTP header in order to be used for splitting 
a consequent media stream amongst several subflow. 
Thus it is used for transmitting a coherent media 
stream on different path parallely. 

**The Latest Version**

Details of the latest version can be found at 
https://github.com/multipath-rtp/gst-mprtp.

**Installation**

gst-mprtp is a gstreamer plugin. In order to use it
you need gstreamer, gst-plugins-base and gst-plugins-good.
For pipelining details see tests/server and client 
applications.
  
If you have the requirements then simply give the
sudo make install command in bash.
  
**Tests**

For tests you need tc and you need to run the following scripts:
  
0. Run make install at gst-mprtp/ directory. 
 
1. Enter to gst-mprtp/tests directory in sudo su mode.

2. Run ./scripts/setup_testbed.sh from tests directory.

3. Run ./server --info 

4. Run ./scripts/run_stest --profile X --duration Y, where X is the profile num for server and client and Y is the duration time for running in seconds. You can abort the test by pressing Ctrl+C

**Contacts**

Balázs Kreith, Varun Singh, Jörg Ott
     
**Acknowledgements** 
  
Special thanks to Jesus Llorente Santos for writing 
the original test scripts. 
  
