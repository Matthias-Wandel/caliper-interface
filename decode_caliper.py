#! /usr/bin/python3
# Decode syncronous serial from cheap chinese calipers
# This python program is barely fast enough on a Rapsberry Pi 4 to bit bang decode
# the syncronous serial.  Use of the C version is recommended.


# Pinout of header connector on calipers, left to right,
# Looking at top of PCB with the edge connector traces facing you:
#
# 1  1.5 volt (connects to battery)
# 2  Clock
# 3  Data
# 4  Ground

# caliper outputs two 24 bit words with syncronous serial at 76.8 kbaud.

# Pinout of header connector to Pi:
#
# 1  3.3 volt
# 2  Clock
# 3  data
# 4  Ground
#
# A voltage divider with 1.3k to ground, 2k across supply of caliper
# and 1k to +3.3 volts puts the 1.5 volt supply if the caliper so that it
# straddles the 1.8 volts hi/low GPIO input threshold

# Gnd ---\/\/\/----\/\/\/--+--\/\/\/--+--\/\/\/---- 3.3V
#          1K       330    |    2K    |    1K
#                          |          |
#                          +- 1.5 V --+
#                           To calliper

# Calliper puts out two 24 bit words, syncronous serial, 76.8 kbits per second. 
# These are LSB first, 20480 increments per inch.  First word reading
# with zero as position where it was turned on.  Second value is the negative
# of what is on the display.  Pushing zero on the caliper resets this values
# to zero for the current position.

import time, sys
import RPi.GPIO as GPIO
GPIO.setmode(GPIO.BCM)
                            
power = 10    # Pin 19   provides power by setting it high.
clockline = 9 # Pin 21   Syncronous serial clock line
dataline = 11 # Pin 23   Syncronous serial data line
#ground       # Pin 25   Ground pin on the Pi.

GPIO.setup(power, GPIO.OUT, initial=GPIO.HIGH) # power provided by output line.
GPIO.setup(clockline, GPIO.IN) # Clock
GPIO.setup(dataline, GPIO.IN)  # Data

num_periods = 0
while True:
    rx_words = [0,0]
    num_words = 0
    errors = 0
    
    while GPIO.input(clockline) == 0: pass # Wait for high
         
    count = 0
    while GPIO.input(clockline) == 1: # See how long it stays high
        count += 1

    if count < 50: # Adjust threshold for other than Pi 4.
        print("start too short")
        errors |= 0x10000
        continue

    while num_words < 2:
        bitval = 1
        value = 0
        while bitval < 0x10000000:
            if GPIO.input(dataline): value = value + bitval
            if GPIO.input(clockline):
                errors += 1
                break
            
            count = 0
            while GPIO.input(clockline) != 1: # Wait for high
                count += 1

            bitval <<= 1                        
                
            count = 0
            while GPIO.input(clockline) != 0: # Wait for low again
                count += 1
                
            if count > 50: 
                if bitval != 0x1000000:
                    errors += 0x100
                    
                rx_words[num_words] = value
                num_words += 1
                break
    
    if errors:
        print("Error: %04x"%(errors))
        continue
        
    mm = [0,0]
    for n in range(2):
        v = rx_words[n] & 0xffffff
        if v & 0x800000: v -= 0x1000000 # Sign extend
        mm[n] = v * 0.00124023 # Caliper values are 20480 increments per inch.

    mm[1] = -mm[1] # Second value is negative
      
    print ("i1=%08x i2=%08x  mm=%7.3f  mm(disp)=%7.3f"%tuple(rx_words + mm))
    




