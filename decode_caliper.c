// Decode digital caliper synchronous serial output on Raspberry Pi
// by Matthias Wandel
// https://github.com/Matthias-Wandel/caliper-interface
//
// This developed and tested on a Raspberry Pi 3A+.  Should run fine on
// anything faster.  Timing primarily limited by calls to teh I/O library,
// so this code may well run faster on a Pi Pico as it doesn't have a
// system call overhead.
//
// This assumes calipers that send postion as two 24 bit words at 77 k baud.
// Cheap calipers either use this scheme or they send one 24 bit value which is
// encoded as 0.01 mm increments.  This assumes the former.

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

// caliper outputs two 24 bit words with synchronous serial at 76.8 kbaud.

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
//
// For electrically noisy enviroments, a .1 uF cap between ground and
// caliper's plus terminal helps.

// Calliper puts out two 24 bit words, synchronous serial, 76.8 kbits per second.
// These are LSB first, 20480 increments per inch.  First word is an absolute
// position reading  where it was turned on, typically starting around 7430000
// However, this value modulo 5 mm is in fact absolute even across power down.

// The Second value is the negative
// of what is on the display.  Pushing zero on the caliper resets this values
// to zero for the current position.


#include <stdio.h>
#include <stdlib.h>
#include <pigpio.h>
#include <unistd.h>
#include <signal.h>


#define POWER_GPIO 10     // Pin 19
#define CLOCK_GPIO 9      // Pin 21
#define DATA_GPIO 11      // Pin 23
//Ground pin              // Pin 25


// Error value is divided into bit fields and can contain multiple errors
#define ERR_CL_GLITCH_COUNT       0x00000001 // 8 bits for clock count
#define ERR_DL_GLITCH_COUNT       0x00000100 // 8 bits for data glitch count (less likely to detect these)
#define ERR_TOO_LATE_FOR_CLOCK    0x00010000 // Possibly missed clock period (Pi is too slow)
#define ERR_START_TOO_SHORT       0x00100000
#define ERR_START_TOO_LONG        0x00200000
#define ERR_WRONG_BIT_COUNT       0x00400000 // Mostlikely due to missed clock periods from not being real-time.
#define ERR_CLOCK_STUCK_LOW       0x01000000 // Clock is not changing at all
#define ERR_TIMEOUT               0x40000000

//-----------------------------------------------------------------------------
// GPIO setup
//-----------------------------------------------------------------------------
void setupGPIO() {
    if (gpioInitialise() < 0) {
        fprintf(stderr, "Failed to initialize pigpio\n");
        exit(1);
    }
    gpioSetMode(POWER_GPIO, PI_OUTPUT);
    gpioWrite(POWER_GPIO, PI_HIGH);
    gpioSetMode(CLOCK_GPIO, PI_INPUT);
    gpioSetMode(DATA_GPIO, PI_INPUT);
}

//-----------------------------------------------------------------------------
// Signal handler to do pigpioTerminate on control-c
//-----------------------------------------------------------------------------
void handle_sigint(int sig) {
    printf("\nCaught signal %d, shutting down pigpio...\n", sig);
	// Clean up pigpio before exiting.  If we don't do this, pigpio may end
	// up in a messed up state, requiring a reboot before it works again.
    gpioTerminate();
    exit(0);
}

//-----------------------------------------------------------------------------
// Wait for clock to change state
//-----------------------------------------------------------------------------
unsigned WaitClockChangeTo(int StateToWait, int min_n)
{
	int n = 0;
	int errors = 0;

	for(int maxr=0;;maxr++){
		if (gpioRead(CLOCK_GPIO) == StateToWait){
			n += 1;
			if (n >= min_n){
				if (maxr <= min_n){
					// Clock was already in that state (glitches notwithstanding),
					// we may be a bit late.
					errors += ERR_TOO_LATE_FOR_CLOCK;
				}
				break;
			}
		}else{
			if (n){
				n=0;
				errors += ERR_CL_GLITCH_COUNT;
			}
		}

		if (maxr >= 10000000){
			errors |= ERR_TIMEOUT;
			break;
		}
	}
	return errors;
}

//-----------------------------------------------------------------------------
// Decode the two 24 bit words coming from caliprs.  These are syncronous serial
// at 76 kbaud, in units of 20480 per inch.
//-----------------------------------------------------------------------------
int BitBangCaliperSerial(int * rx_words)
{
    int errors = 0;
    int num_words = 0;
	int LastClockFlip = gpioTick();

	int t;
	int duration;

	while (1){
		LastClockFlip = gpioTick();
		// Clock line floats between transmissions, specify that we want to
		// see it high for much longer before we accept it as the starting clock high.
		errors += WaitClockChangeTo(1,20);
		duration = gpioTick() - LastClockFlip;
		if (duration > 400000){
			// should see something at least 3x per second.
			errors |= ERR_CLOCK_STUCK_LOW;
			return errors;
		}

		LastClockFlip = gpioTick();
		errors += WaitClockChangeTo(0,3);
		duration = gpioTick()-LastClockFlip;
		if (duration <= 1){
			// Going high this briefly is most likely a glitch.
			errors += ERR_CL_GLITCH_COUNT;
		}else{
			break;
		}
	}

	if (duration < 45){
		errors |= ERR_START_TOO_SHORT;
		//printf("Start too short %d\n", duration);
		return errors;
	}

	if (duration > 60){
		errors |= ERR_START_TOO_LONG;
		//printf("Start too long\n");
		return errors;
	}
	errors = 0; // We can accumulate a lotof "glitches" waiting for start, so reset that.
	while (num_words < 2) {
		int value = 0;
		int bitval = 1;

		while (bitval < 0x10000000) {

			const int num_average = 3; // Could do more reads on pi 4 or 5
			int sum = 0;
			for (int i=0;i<num_average;i++) sum += gpioRead(DATA_GPIO);

			if (sum > num_average/2) value |= bitval;
		    bitval <<= 1;
			if (sum > 1 && sum < num_average){
				// Glitches on the data line are more worriesome, so count those as 4x.
				errors += ERR_DL_GLITCH_COUNT;
			}

			errors += WaitClockChangeTo(1,3);
			if (errors & ERR_TIMEOUT) break;
			LastClockFlip = gpioTick();
			errors += WaitClockChangeTo(0,3);
			if (errors & ERR_TIMEOUT) break;
			int duration = gpioTick()-LastClockFlip;

			if (duration > 20) {
				if (bitval != 0x1000000){
					//printf("wrong bit count %08x\n",bitval);
					errors += ERR_WRONG_BIT_COUNT;
				}

				value &= 0xFFFFFF;
				if (value & 0x800000) value -= 0x1000000; // Sign extend the 24 bit value

				rx_words[num_words] = value;
				num_words += 1;
				break;
			}
		}
    }
	return errors;
}

//-----------------------------------------------------------------------------
// Mainline
//-----------------------------------------------------------------------------
int main(int argc, char *argv[])
{
    // Check if there is at least one argument passed
	int SingleReadingMode = 0;
	int OffAfter = 0;
	int NumAvg = 1;
	for (int n=1;n<argc;n++){
        // Compare the first argument with "-s"
        if (argv[n][0] == '-'){
			if (argv[n][1] == 's') SingleReadingMode = 1;
			if (argv[n][1] == 'o') OffAfter = 1;
		}
	}

    setupGPIO();
    // Register signal handler to do gpioTerminate on Ctrl+C
    signal(SIGINT, handle_sigint);  // Stop via control-C
	signal(SIGTERM, handle_sigint); // Stpo via kill command
	signal(SIGTSTP, handle_sigint);  // Suspend with ctrl-Z - just quit

	if (OffAfter && SingleReadingMode == 0){
		printf("Turn off caliper supply\n");
		gpioWrite(POWER_GPIO, PI_LOW);
		gpioTerminate();
		return 0;
	}

	while (1){
		int rx_words[2];
		int errors = BitBangCaliperSerial(rx_words);
		if (errors > 0x100000){
			printf("Decode fail, error %x",errors);
			if (errors & ERR_START_TOO_LONG) printf(",  start too long");
		    if (errors & ERR_WRONG_BIT_COUNT) printf(",  Wrong bit count");
		    if (errors & ERR_CLOCK_STUCK_LOW) printf(",  Clock stuck low");
			if (errors ^ ERR_TIMEOUT) printf(",  Clock timeout");
			putchar('\n');

			// Larger errors mean decoding must have failed.
			// sleep past of rest of the serial message before trying again.
			usleep(1500);
			continue;
		}

        double mm[2];
        for (int n = 0; n < 2; n++) {
            mm[n] = rx_words[n] * (25.4/20480); // Convert to mm (reads 20480 increments per inch)
        }
        mm[1] = -mm[1]; // Second value is negative

		//printf("i1=%08x i2=%08x  ", rx_words[0], rx_words[1]);
		printf("i1=%8d i2=%8d  ", rx_words[0], rx_words[1]);
        printf("Abs:%8.3fmm  Disp:%8.3fmm", mm[0], mm[1]);

		if (errors){ // Print late and glitch counts
			printf(" L:%d Gl:%2d,%d ",(errors >> 16) & 0x0f, errors & 0xff, (errors >> 8) & 0xff);
			for (int a=0;a<(errors & 0xff);a++) putchar('g'); // Glitch count bargraph
			for (int a=0;a<(errors & 0xff00);a+= 0x100) putchar('D'); // Glitch count bargraph
			
		}
		putchar('\n');

		if (SingleReadingMode) break;
		if (errors == 0){
			// After successful decode, safe to sleep till next one should come.
			usleep(300000); // Only 3 readings per second, so might as well sleep
							// until we get close to the next one.
		}
    }

	if (OffAfter){
		printf("Turning off caliper supply\n");
		gpioWrite(POWER_GPIO, PI_LOW);
	}

    gpioTerminate();
    return 0;
}