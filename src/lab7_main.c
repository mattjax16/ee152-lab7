// The pin hookup:
//	- analog ECG input data comes in on PA0, or Nano A0.
//	- drive out a canned ECG signal, if desired, on DAC 1 (PA4, or Nano A3)
//	  (and then jumper A3 -> A0).
//	- drive out debug information on DAC 2 (PA5, or Nano A4).
//	- USART1 drives PA9 (Nano D1), which drives the LCD display.
//	- GPIO PA12 (Nano D2) which drives the buzzer.

// Include FreeRTOS headers.
#include "FreeRTOSConfig.h"
#include "FreeRTOS.h"
#include "portmacro.h"
#include "task.h"
#include "timers.h"

#include "stm32l4xx.h"
#include "stm32l432xx.h"
#include <stdbool.h>
#include "lib_ee152.h"

// Dual_QRS indicates that both the left & right side of the algorithm believe
// we have a QRS, and that we're not in the refractory period.
// Dual_QRS_last is just a one-cycle-delayed version of dual_QRS; it's used to
// detect the rising edge of dual_QRS.
static bool dual_QRS = false;
static bool dual_QRS_last = false;

//****************************************************
// Biquad filtering.
//****************************************************

struct biquadcoeffs {	// The coefficients of a single biquad section.
    float b0, b1, b2,	// numerator
	  a0, a1, a2;	// denominator
};
// Our 20Hz lowpass filter is built from two biquad sections.
#define N_BIQUAD_SECS 2	// Number of biquad sections in our filter.
static struct biquadcoeffs biquad_20Hz_lowpass[2] = {
	{8.59278969e-05f, 1.71855794e-04f, 8.59278969e-05f,
	 1.0f,		-1.77422345e+00f, 7.96197268e-01f},
	{1.0f,	2.0f,	1.0f,
	 1.0f,	-1.84565849e+00f,	9.11174670e-01f}};

// All DSP filters need state.
struct biquadstate { float x_nm1, x_nm2, y_nm1, y_nm2; };
static struct biquadstate biquad_state[N_BIQUAD_SECS] = {0};

// Biquad filtering routine.
// - The input is assumed to be a 12-bit unsigned integer coming straight from
//   the ADC. We convert it immediately to a float xn in the range [0,1).
// - Compute yn = b0*xn + b1*x_nm1 + b2*x_nm2 - a1*y_nm1 - a2*y_nm2
// - Update x_nm1->x_nm2, xn->x_nm1, y_nm1->y_nm2, yn->y_nm1
// - Return yn as a 12-bit integer.
int biquad (const struct biquadcoeffs *coeffs, struct biquadstate *state,
	    uint32_t sample, uint32_t n_bits) {
    float xn = ((float)sample) / ((float)(1<<n_bits)); // current sample
    float yn = coeffs->b0*xn + coeffs->b1*state->x_nm1 + coeffs->b2*state->x_nm2
	     - coeffs->a1*state->y_nm1 - coeffs->a2*state->y_nm2; // output

    state->x_nm2 = state->x_nm1;
    state->x_nm1 = xn;
    state->y_nm2 = state->y_nm1;
    state->y_nm1 = yn;

    return (yn * (1<<n_bits));
}

//****************************************************
// Calculate a derivative with a fancy five-point algorithm.
//****************************************************

// Compute derivative using a 5-point algorithm.
// Given an input 'sample', keep track of its last 5 values, which they call
// xp2, xp1, x0, xm1 and xm2. Then when we get a new sample, compute & return
//	(-xm2 - 2*xm1 + 2*xp1 + xp2)/8
// and then shift to do xm2=xm1, xm1=x0, x0=xp1, xp1=xp2, xp2=sample
// So you can think of this as implementing a differentiator with a delay of
// two time units.
struct deriv_5pt_state {
    int xp2, xp1, x0, xm1, xm2;
};

struct deriv_5pt_state deriv_5pt_state = { 0 };
struct deriv_5pt_state deriv_5pt_state1 = { 0 };
struct deriv_5pt_state deriv_5pt_state2 = { 0 };

int deriv_5pt (int sample, struct deriv_5pt_state *state) {
    int r = -state->xm2 - 2*state->xm1 + 2*state->xp1 + state->xp2;
    r = r>>3;	// Divide by 8.

    state->xm2 = state->xm1;
    state->xm1 = state->x0;
    state->x0 = state->xp1;
    state->xp1 = state->xp2;
    state->xp2 = sample;

    return (r);
}

//****************************************************
// Windowing algorithm
//****************************************************

// Just a running average of the last 100 samples, using a circular buffer.
// Used by the right-side algorithm.
#define WINDOW_SIZE 100 // samples for the running average.
static int window_ravg (int sample) {
    static int window_buf [WINDOW_SIZE] = { 0 };
    static int window_ptr = 0;
    static long window_sum = 0;

    window_sum -= window_buf[window_ptr];
    window_buf[window_ptr] = sample;
    window_sum += sample;
    window_ptr = (window_ptr+1) % WINDOW_SIZE;

    return (window_sum / WINDOW_SIZE);
}

//****************************************************
// The moving-threshold algorithm, used as the near-final stage of the left-side
// and right-side calculations.
//****************************************************

struct threshold_state {
    int threshold; // running peak threshold
    int max, min;
    int decay; // amount that max and min thresholds decay each sample
};
struct threshold_state threshold_state_1 = { 0x7FF, 0x000, 0xFFF, 15 };
struct threshold_state threshold_state_2 = { 0x7FF, 0x000, 0x2FF, 4 };

// The moving-threshold algorithm.
// It always keeps a running min & running max. Each time we get a new sample
// (and hence call this function)...
//    - A negative sample is the exception; immediately return the current
//	threshold (which is (min+max)/2).
//    - The new sample goes into the running min & max.
//    -	The max decrements by a fixed delta, and the min increments by the
//	same fixed delta. Clamp the max to never go <0, and the min to never
//	go >0xFFF.
//    - Return (min + max)/2
// Does it really make sense to have max < min??? This algorithm allows that!
// And note that this algorithm is completely different than Pan Tompkins.
int threshold( struct threshold_state * state, int psample ) {
    if (psample <= 0) return (state->threshold); // no sample peak

    if (psample > state->max) state->max = psample;
    if (psample < state->min) state->min = psample;

    // Implement decay.
    state->max -= state->decay;
    if (state->max < 0x000) state->max = 0x000;
    state->min += state->decay;
    if (state->min > 0xFFF) state->min = 0xFFF;

    state->threshold = (state->min + state->max)/2;
    return (state->threshold);
}

struct compute_peak_state {
    struct deriv_5pt_state der5_state;
    int prev_deriv;
};

// Usually return 0; but when the input signal hits a peak, then return the
// value of the signal (i.e., of the peak).
int compute_peak (int sample, struct compute_peak_state *state) {
    // First compute the derivative.
    int deriv = deriv_5pt (sample, &state->der5_state);

    // Peak==1 if the current sample fell and the previous one rose. I.e., we
    // just had a peak.
    bool peak = (state->prev_deriv>=0) && (deriv<0);
    state->prev_deriv = deriv; // update derivative history

    // We'll usually return 0, but data if we just had a peak.
    return (peak? sample : 0);
}

#define READ_WRITE_DELAY ( 2 / portTICK_PERIOD_MS ) // sample at 500 Hz
#define REFRACTORY_TICKS 100 // 200 ms at 500 Hz
// Schedule this task every 2ms.
void task_main_loop (void *pvParameters) {
    int sample_count=0;	// To ignore startup artifacts.
    // Refractory_counter is zeroed at dual_QRS falling edge, and counts up each
    // tick after that. It's to ignore new peaks too close to an existing one.
    int refractory_counter = 0;
    struct compute_peak_state peak_state_1, peak_state_2;

    for ( ;; ) {
	vTaskDelay (READ_WRITE_DELAY);

	// Read ADC, using a spin-wait loop.
	uint32_t sample = analogRead (A0);
	//sample *= 2;
	uint32_t output = sample >> 4;
	//output *= 0.5;
	analogWrite (A4, output);

	// Run it through one or more cascaded biquads.
	int filtered = sample;
	for (int i=0; i<N_BIQUAD_SECS; ++i)
	    filtered = biquad(&biquad_20Hz_lowpass[i],
			      &biquad_state[i], filtered, 12);

	// Left-side analysis
	// Peak_1 is usually 0; but when the bandpass-filtered signal hits a
	// peak, then peak_1 is the bandpass-filtered signal.
	int peak_1   = compute_peak (filtered, &peak_state_1);
	int thresh_1 = threshold (&threshold_state_1, peak_1);

	// Right-side processing
	// Fancy 5-point derivative of the bandpass-filtered signal.
	int deriv_2 = deriv_5pt (filtered, &deriv_5pt_state);
	int deriv_sq_2 = deriv_2 * deriv_2;

	// Running_avg over a 200ms window.
	int avg_200ms_2 = window_ravg(deriv_sq_2);

	// Right-side analysis
	int peak_2 = compute_peak (avg_200ms_2, &peak_state_2);
	if (++sample_count < 250) continue;
	int thresh_2 = threshold (&threshold_state_2, peak_2);

	// Dual-QRS calculation combining left & right sides.
	dual_QRS_last = dual_QRS;	// pipe stage for edge detect.
	++refractory_counter;
	dual_QRS = (filtered > thresh_1) && (avg_200ms_2 > thresh_2)
		&& (refractory_counter > REFRACTORY_TICKS);
	if (dual_QRS_last && !dual_QRS) refractory_counter = 0;
	// Write to DAC 2, which drives Nano pin A4.
	//analogWrite (A4, dual_QRS);
    }
}

#define BLINK_GRN_DELAY ( 500 / portTICK_PERIOD_MS )
void task_blink_grn (void *pvParameters) {
    bool value = 0;
    for ( ;; ) {
	// The green LED is at PB3, or Nano D13.
	digitalWrite (D13, value);
	value = !value;
	vTaskDelay (BLINK_GRN_DELAY);
    }
}

// 250Hz beep.
// Flag the dual_QRS leading edge. When that happens...
// - Flip the GPIO pin every 4 ticks (4ms)
// - Stop after 100 ticks.
// So we get an 8ms period (125Hz) beep for 100ms.
void task_beep (void *pvParameters) {
    int val=0;
    int dual_QRS_last=0;
    int beep_counter=0;
    for ( ;; ) {
	if (dual_QRS && !dual_QRS_last)
	    beep_counter = 1;
	// Flip the GPIO pin every 4 ticks. We write a 1 at counter=4, 0 at
	// counter=8, etc.
	if ((beep_counter>0) && (beep_counter&0x3)==0) {
	    val = !val;
	    digitalWrite (D2, val);	// The buzzer is on Nano D2, or PA12.
	}
	// And turn off the beep after 100 ticks.
	if ((beep_counter>0) && (beep_counter++==96))
	    beep_counter = 0;
	dual_QRS_last=dual_QRS;
	vTaskDelay(1);
    }
}

void USART_write_byte (unsigned char c) {
    static char buf[2];
    buf[0]=c;
    buf[1]='\0';
    serial_write (USART1, buf);
}

// Output a float in [0,999.9] to a 4-digit LCD.
static void float_to_LCD (float f) {
    // Round to fixed point. Three digits to the left of the decimal point,
    // and one to the right.
    int number = f*10 + .5;

    USART_write_byte (0x76);		// Clear display, set cursor to pos 0

    if (number > 9999) {			// Detect overflow (print "OF").
	USART_write_byte ('0');
	USART_write_byte ('F');
	return;
    }

    // We have an integer in [0,9999]. Output the four digits, MSB first.
    int power[4]={1,10,100,1000};	// so power[i] = 10**i.
    int all_zeros_so_far=1;		// True iff outputted all zeros so far.
    for (int pos=3; pos >=0; --pos) {
	int add=power[pos], sum=0;
	for (int i=0;; ++i) {
	    sum += add;
	    if (sum > number) {
		number -= (sum-add);
		all_zeros_so_far &= (i==0);
		// Output the digit (i);
		if (all_zeros_so_far)
		    USART_write_byte (0x20);	// Space, not leading 0
		else
		    USART_write_byte ('0'+i);

		break;	// on to the next LSB-most position.
	    }
	}
    }
    USART_write_byte (0x77);	// Command to write decimal point(s) or colon...
    USART_write_byte (0x04);	// ... and write the 2nd decimal point
}

void task_displaybpm(void *pvParameters) {
    static TickType_t last_new_beat=0;
    for ( ;; ) {
	if (!dual_QRS_last && dual_QRS) {	// for every *new* heartbeat...
	    TickType_t time = xTaskGetTickCount();
	    // Convert time in milisec to beats/minute.
	    float bpm = 60.0f * 1000.0f / (time - last_new_beat);
	    float_to_LCD (bpm);
	    last_new_beat = time;
	}
	vTaskDelay (1);
    }
}

#define TICKS_PER_PT 2	// Typically 500 Hz sampling, so TICKS_PER_PT=2
#define ECG_DATA_FILE "ecg_normal_board_calm1.txt"
static unsigned short int ECG_data[] = {
#include ECG_DATA_FILE
};

// Write a canned ECG out on DAC 1, which drives PA4 (Nano A3).
// Note that the ADC is 12 bits but the DAC is 8 bits, so we do a 4-bit right
// shift before the analog write.
void task_canned_ECG (void *pvParameters) {
    int n_datapoints = (sizeof ECG_data) / (sizeof (short int));

    int i=0;
    while (1) {
	if (++i == n_datapoints)
	    i = 0;
	//unsigned int data = (ECG_data[i]-1300) >> 3;
	unsigned int data = ECG_data[i] >> 4;
	analogWrite (A3, data);
	if (data > 0xFF) {
	    analogWrite (A3, 0xFF);
	    error ("Canned data is out of range");
	}
	vTaskDelay(TICKS_PER_PT);
    }
}

int main() {
    clock_setup_80MHz();

    // The green LED is at Nano D13, or PB3.
    pinMode(D13, "OUTPUT");
    digitalWrite (D13, 0);	// Turn it off.

    // Set up piezo GPIO. It's Nano D2, or PA12
    pinMode(D2, "OUTPUT");
    pinMode(D6, "OUTPUT");

    // We use the UART to talk to the 7-segment display. Initialize the UART,
    // and kick off the display with any old value.
    serial_begin (USART1);
    float_to_LCD (40.2);

    // Create tasks.
    TaskHandle_t task_handle_grn = NULL;
    BaseType_t status = xTaskCreate	(
	task_blink_grn, "Blink Red LED",
	128, // stack size in words
	NULL, // parameter passed into task, e.g. "(void *) 1"
	tskIDLE_PRIORITY+2, // priority
	&task_handle_grn);
    if (status != pdPASS) error ("Cannot create blink-green task");

    TaskHandle_t task_handle_main_loop = NULL;
    status = xTaskCreate (
	task_main_loop,
	"Main loop",
	256, // stack size in words
	NULL, // parameter passed into task, e.g. "(void *) 1"
	tskIDLE_PRIORITY+1, // priority
	&task_handle_main_loop);
    if (status != pdPASS) error ("Cannot create main-loop task");

    TaskHandle_t task_handle_beep = NULL;
    status = xTaskCreate (
	    task_beep, "Beep Piezo Buzzer",
	    100, // stack size in words
	    NULL, // parameter passed into task, e.g. "(void *) 1"
	    tskIDLE_PRIORITY, // priority
	    &task_handle_beep);
    if (status != pdPASS) error ("Cannot create beep task");

    TaskHandle_t task_handle_displaybpm = NULL;
    status = xTaskCreate (
	    task_displaybpm, "Display BPM",
	    100, // stack size in words
	    NULL, // parameter passed into task, e.g. "(void *) 1"
	    tskIDLE_PRIORITY, // priority
	    &task_handle_displaybpm);
    if (status != pdPASS) error ("Cannot create display-BPM task");

    TaskHandle_t task_handle_canned_ECG = NULL;
    status = xTaskCreate (
	task_canned_ECG, "Task to drive a canned ECG out to PA3",
	100, // stack size in words
	NULL, // parameter passed into task, e.g. "(void *) 1"
	tskIDLE_PRIORITY+2, // priority
	&task_handle_canned_ECG);
    if (status != pdPASS) error ("Cannot create drive-ECG task");

    vTaskStartScheduler();
}