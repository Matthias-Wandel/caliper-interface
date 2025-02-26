// Decode digital caliper syncronous serial output on Raspberry Pi
// by Matthias Wandel
// https://github.com/Matthias-Wandel/caliper-interface
//
// Compiling:
//    gcc -o decode_caliper decode_caliper.c -lpigpio
//
// Must run as root.


// Pinout of header connector on calipers, left to right,
// Looking at top of PCB with the edge connector traces facing you:
//
// 1  1.5 volt (connects to battery)
// 2  Clock
// 3  Data
// 4  Ground

// caliper outputs two 24 bit words with syncronous serial at 76.8 kbaud.

// Pinout of header connector to Pi:
//
// 1  3.3 volt
// 2  Clock
// 3  data
// 4  Ground
//
// A voltage divider with 1.3k to ground, 2k across supply of caliper
// and 1k to +3.3 volts puts the 1.5 volt supply if the caliper so that it
// straddles the 1.8 volts hi/low GPIO input threshold

// Gnd ---\/\/\/----\/\/\/--+--\/\/\/--+--\/\/\/---- 3.3V
//          1K       330    |    2K    |    1K
//                          |          |
//                          +- 1.5 V --+
//                           To calliper

// Calliper puts out two 24 bit words, syncronous serial, 76.8 kbits per second. 
// These are LSB first, 20480 increments per inch.  First word reading
// with zero as position where it was turned on.  Second value is the negative
// of what is on the display.  Pushing zero on the caliper resets this values
// to zero for the current position.


#include <stdio.h>
#include <stdlib.h>
#include <pigpio.h>
#include <unistd.h>

#define POWER_PIN 10     // GPIO 19
#define CLOCK_PIN 9      // GPIO 21
#define DATA_PIN 11      // GPIO 23

void setupGPIO() {
    if (gpioInitialise() < 0) {
        fprintf(stderr, "Failed to initialize pigpio\n");
        exit(1);
    }
    gpioSetMode(POWER_PIN, PI_OUTPUT);
    gpioWrite(POWER_PIN, PI_HIGH);
    gpioSetMode(CLOCK_PIN, PI_INPUT);
    gpioSetMode(DATA_PIN, PI_INPUT);
}

unsigned volatile int LastClockFlip;

unsigned volatile int t1, t2, t3;
//-----------------------------------------------------------------------------
// Wait for clock to change state
//-----------------------------------------------------------------------------
unsigned WaitClockChangeTo(int StateToWait)
{
	int t;
	for(;;){
		t = gpioTick();

		if (gpioRead(CLOCK_PIN) == StateToWait) break;

		if (t-LastClockFlip > 1000){
			// Timeout.
			LastClockFlip = t;
			return 0;
		}
	}
	int duration = t-LastClockFlip;
	LastClockFlip = t;
	return duration;
}


//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
int main()
{
    setupGPIO();

    while (1) {
        int errors = 0;
        int rx_words[2] = {0, 0};
        int num_words = 0;
		LastClockFlip = gpioTick();

		int t;

		while (gpioRead(CLOCK_PIN) == 0){ // Wait for high
			t = gpioTick();

			if (t - LastClockFlip > 400000){
				// Timeout.
				printf("Not seeing clock go high\n");
			}
		}

		if (t - LastClockFlip >= 400000){
			printf("timed out waiting for clock high\n");
			continue;
		}

		if (t - LastClockFlip <= 2000){
			// Didn't have 2 ms of low, wait again.
			printf("No 2 ms of low, try again\n");
			continue;
		}

		LastClockFlip = gpioTick();
		int start_duration = WaitClockChangeTo(0);

		if (start_duration < 45){
			errors |= 0x10000;
			printf("Start too short %d\n", start_duration);
			continue;
		}

		if (start_duration > 60){
			errors |= 0x10000;
			printf("Start too long\n");
			continue;
		}

		while (num_words < 2) {
			int value = 0;
			int bitval = 1;

			while (bitval < 0x10000000) {
				if (gpioRead(DATA_PIN)) {
					value |= bitval;
				}
			    bitval <<= 1;

				if (gpioRead(CLOCK_PIN)) {
					// Low clock period (bit valid period) may have ended while we read.
					printf("Missed clock low period\n");
					errors += 0x01;
					break;
				}

				WaitClockChangeTo(1);
				int duration = WaitClockChangeTo(0);

				if (duration > 20) {
					if (bitval != 0x1000000){
						printf("wrong bit count %08x\n",bitval);
						errors += 0x100;
					}
					rx_words[num_words] = value;
					num_words += 1;
					break;
				}
			}
        }

        if (errors) {
            printf("Error: %04x\n", errors);
            continue;
        }

        double mm[2];
        for (int n = 0; n < 2; n++) {
            int32_t v = rx_words[n] & 0xFFFFFF;
            if (v & 0x800000) v -= 0x1000000; // Sign extend
            mm[n] = v * 0.00124023; // Convert to mm (reads 20480 increments per inch)
        }
        mm[1] = -mm[1]; // Second value is negative

		printf("i1=%08x i2=%08x  ", rx_words[0], rx_words[1]);
        printf("Abs:%7.3fmm  disp:%7.3fmm\n", mm[0], mm[1]);
		usleep(1000);
    }

    gpioTerminate();
    return 0;
}