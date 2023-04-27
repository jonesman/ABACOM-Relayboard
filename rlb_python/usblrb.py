#
# Updated version for Python 3, 01.04.2023
# Just a few syntax changes were needed
# Original version as found on the ABACOM forums (linked from product page):
# https://forum.abacom-online.de/phpBB3/viewtopic.php?f=51&t=3751
#
# updated by Jonas Keunecke (drjones16@web.de)
# https://github.com/jonesman/ABACOM-Relayboard
# Original description below:

#######################################################################

###        ABACOM USB-LRB relay board (Python 2 script)
###        to set relays status from linux command line
###      tested on Raspberry Pi Rev.2 (Linux)  ABACOM 11/2013 

###    Other linux systems were not tested. Feedback wellcome.

###                !!! USE ON YOUR OWN RISK !!!

###       You need an active USB-HUB (or external) power 
###   for the relay board! RASPI USB POWER is NOT strong enough!

#######################################################################

#######################################################################
### usage: sudo usblrb.py -d <deviceno> -s <status>
#######################################################################

### example calls from linux command line:

### to change relays status ...
###     sudo python ./usblrb.py --device 0 --status 123  
### or
###     sudo python ./usblrb.py -d 0 -s 123 

### to list all devices ...
###     sudo python ./usblrb.py

### to read relays status from a devices ...
###     sudo python ./usblrb.py -d 0

### DEVICE: zero-based CH341A device list index 
### STATUS: Bit 0..7 represent REL1..REL8 status:
### bit0..7 set = relay1..8 on
### bit0..7 clear = relay1..8 off

###################### required libraries  #######################
### We need to have "pyUSB" library installed to access USB endpoints directly.
### see http://pyusb.sourceforge.net/docs/1.0/tutorial.html
### pyUSB requires the "libusb10" as "backend".
### see http://www.libusb.org/wiki/libusb-1.0

### Do NOT ask me how to install these libs.
### It may be tricky, but somehow you will succeed!
### Google for it!

import usb.core
import usb.util

### These libs likely are already pre-installed on RaspPi...
import sys
import time
import getopt

############# (Some of) the CH341A API ####################

### global "USB device" object
dev = None # we will get and use it later

### from CH341A USB Descriptor... on Linux type: "lsusb -v"
interface = 0 # CH341A only has one interface 0
ep2out = 0x02 # CH341A Endpoint for outgoing data (PC->CH341A)
ep2in  = 0x82 # CH341A Endpoint for incoming data (CH341A->PC)

# CH341A low level transfers not documented unfortunately,
# so captured from Windows driver using USBPcap & Wireshark
# see http://desowin.org/usbpcap/

ch341a_set_output = 0xA1
ch341a_get_input = 0xA0

### CH341A API function
def setOutput(DataByte): 
    ### Message bytes were captured from WIN driver DLL.
    ### NOT ask me what these bytes mean in detail!
    global dev
    msg = bytearray()
    msg.append(ch341a_set_output)
    msg.append(0x6a)
    msg.append(0x1f)
    msg.append(0x00)
    msg.append(0x10)
    msg.append(DataByte)
    msg.append(0x3f)
    msg.append(0x00)
    msg.append(0x00)
    msg.append(0x00)
    msg.append(0x00)
    dev.write(ep2out,msg,0)

### CH341A API function
def getInput():
    global dev
    msg = bytearray()
    msg.append(ch341a_get_input)
    dev.write(ep2out,msg,0)
    response=dev.read(ep2in,6)
    return response 

############## USB LRB relay specific functions ##########

### Allegro A6275 driver chip is on CH341A data lines...
LATCH =  0x01 # to A6275 Latch in
CLK  =   0x08 # to A6275 CLK in 
DATA =   0x20 # to A6275 Serial in
PFT  =   0x40 # port function test from A6275 PIN5 (REL1)
READ =   0x80 # from A6275 Serial out

### Shift bits from CH341A to Allegro A6275 driver chip...
def shiftOutBits(aStatus):
    setOutput(0) #All lines low
    for i in range(0,8): # Bit 0..7 testen...
        if (aStatus & (1 << (7-i)))!=0 :
            setOutput(DATA) #DATA high "1"
	    # and generate CLK pulse...
            setOutput(CLK or DATA) #CLK high
            setOutput(DATA) #CLK low
        else:
            setOutput(0) #DATA low "0"
	    # and generate CLK pulse...
            setOutput(CLK) #CLK high
            setOutput(0) #CLK low
    setOutput(0) #All lines 0

### Shift out (write / set) the relays status to Allegro A6275
def setRelays(aStatus):
    setOutput(0) # Latch low
    shiftOutBits(aStatus) # this is silent so far (without latch)
    # now generate a latch clock to output data to relays...
    setOutput(LATCH) #Latch high
    setOutput(0) # Latch, CLK, OE low

### Shift in (read/verify) the relays status from Allegro A6275
def getRelays():

    ### Note: After power-up setRelays() must be called first (at least once)!
    ### Otherwise the result of getRelays() may be invalid.
    ### This is bescause the Allegro A6275 data register may
    ### have different state, than its (latched) output register.

    global dev
    result = 0
    
    setOutput(0) # all lines low
    for i in range(0,8): # shift out bit 0..7 from A6275...
        msg = getInput() # CH341A API call
        inputState = msg[0] # Get status of CH341A D0..D7 lines
        # READ bits from A6275 Serial out (at D7 line)...
        if (inputState & READ)!=0: 
           result = result | (1 << (7-i));
        # ...and generate CLK pulse for next bi from A6275t...
        setOutput(CLK) #CLK high
        setOutput(0) #CLK low

    powerFail = 0
    if result == 255:
        powerFail == ((inputState & PFT)!=0)
        if powerFail!=0:
            result = -1

    if powerFail == 0 :
        shiftOutBits(result) # write back status we shifted out before

    return result

################################################################
# Setting the relays could be simply like this for one card...
#
#    dev = usb.core.find(idVendor=0x1A86, idProduct=0x5512) 
#    setRelays(SomeStatus)
#
# But we like to have it nicer,
# so everthing below is getting arguments and error checking...
################################################################

### main method processes the command line ...
def main(argv):
    global dev

    ### Get string args from command line...
    devnoString= ''
    statusString = ''
    try:
        opts, args = getopt.getopt(argv,"hd:s:",["deviceno=","status="])
    except getopt.GetoptError:
        print('usage: sudo usblrb.py -d <deviceno> -s <status>')
        sys.exit(2)
    for opt, arg in opts:
        if opt == '-h':
            print('usage: sudo usblrb.py -d <deviceno> -s <status>')
            sys.exit()
        elif opt in ("-d", "--deviceno"):
            devnoString = arg
        elif opt in ("-s", "--status"):
            statusString = arg
    ### We got the args now, so we can try to use it....


    ### Next we need to find a CH341A DEVICE in EPP/MEM/I2C mode on USB...

    ### Method 1: Use first device found on bus... (not used here)
    # dev = usb.core.find(idVendor=0x1A86, idProduct=0x5512) - not used here
    ### Method 2: Specify bus and address ... (not used here)
    # dev = usb.core.find(bus=1, address=35) - not used here
    ### Method 3: Find all CH341A and pick one from the list ... that is my favourite!
    devs_iter = usb.core.find(find_all=1, idVendor=0x1A86, idProduct=0x5512) # List of all CH341A in EPP/MEM/I2C mode
    if devs_iter is None:
       print('No device found!')
       sys.exit()

    devs = list(devs_iter)
    if devnoString=='': # no device specified in command line
        for i in range(0, len(devs)): # Print out the decive list...
            dev = devs[i]
            print('DEVICE',i,'found at BUS',dev.bus,' ADR', dev.address)
        print('usage: sudo usblrb.py -d <deviceno> -s <status>')
        sys.exit()
    else:
        try:
            devIndex = int(devnoString)
        except ValueError:
            print('Invalid device no.!')
            sys.exit()

        try:
           dev=devs[devIndex] # pick device from list
        except IndexError:
            print('Device not found!')
            sys.exit()

    if dev is None: # still no device? just in case...
        print('Device error!')
        sys.exit()
    ### Hurray! We have a device object now!
    print('DEVICE',devIndex,'found at BUS',dev.bus,' ADR', dev.address)


    ### Now get new STATUS from command line...
    if statusString=='': # no status specified in command line
        # Test if it is a USB-LRB or not
        # and read the current status in case...
        oldStatus = getRelays() # get the relay status
        testStatus = not oldStatus
        shiftOutBits(testStatus) # shift out some other status (silent without latch)
        status = getRelays() # readback new (test status)
        if status == testStatus: # does it match?
           print('Status read: ',oldStatus) # print out the status
           shiftOutBits(oldStatus) # restore status we had before test
        else:
           print('Bad device') # this is likely not a USB-LRB
        sys.exit()

    try:
        newStat = int(statusString)
    except ValueError:
        print('Invalid status value!')
        sys.exit()

    if not (newStat in range(0,256)):
        print('Status value out of range (must be 0..255)')
        sys.exit()
    ### Hurray! We have a status value!
        

    ### Finally we set the new relay status on (global) device ....
    setRelays(newStat)
    print('Status set to', newStat)

    ### if you feel better with that, you can verify... 
    if getRelays()==newStat:
        print('Verified successfully.')
    else:
        print('Verfication failed!')

    

##############################################################
### call the main method with arguments from command line ...
if __name__ == "__main__":
   main(sys.argv[1:])
