#include <limits.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#define ECG_FILE "data/ecg_normal_board_calm1.txt"

// Simulated FreeRTOS functionality
#define portTICK_PERIOD_MS 1
#define tskIDLE_PRIORITY 0
typedef int TickType_t;
static int tick_count = 0;
TickType_t xTaskGetTickCount(void) { return tick_count; }


// Mock analog functions
static FILE* input_file = NULL;
uint32_t analogRead(int pin) {
    static uint32_t value;
    if (fscanf(input_file, "%u,", &value) == 1) {
        return value;
    }
    rewind(input_file);  // Loop the data
    fscanf(input_file, "%u,", &value);
    return value;
}

void analogWrite(int pin, uint32_t value) {
    printf("DAC output pin %d: %u\n", pin, value);
}

// Debug output functions
void plot_point(int time, int value, const char* series) {
    FILE* gnuplot = fopen("plot_data.txt", "a");
    fprintf(gnuplot, "%d %d %s\n", time, value, series);
    fclose(gnuplot);
}

// Keep the core algorithm code unchanged
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


// Main desktop debug loop
int main() {
    int sample_count=0;	// To ignore startup artifacts.
    // Refractory_counter is zeroed at dual_QRS falling edge, and counts up each
    // tick after that. It's to ignore new peaks too close to an existing one.
    int refractory_counter = 0;
    struct compute_peak_state peak_state_1, peak_state_2;
    input_file = fopen(ECG_FILE, "r");
    if (!input_file) {
        printf("Error: Cannot open input file\n");
        return 1;
    }

    // // Initialize plot file
    // FILE* gnuplot = fopen("ECG_FILE", "w");
    // fprintf(gnuplot, "# Time Raw Filtered Derivative ThresholdCrossing\n");
    // fclose(gnuplot);
    int min_deriv = INT_MAX;
    int max_deriv = INT_MIN;
    // Main processing loop
    for (int i = 0; i < 1000; i++) {  // Process 1000 samples for testing
        uint32_t sample = analogRead(0);
        
        // Apply biquad filter
        int filtered = sample;
        for (int j = 0; j < 2; j++) {
            filtered = biquad(&biquad_20Hz_lowpass[j], &biquad_state[j], filtered, 12);
        }

        // Left-side analysis
        // Peak_1 is usually 0; but when the bandpass-filtered signal hits a
        // peak, then peak_1 is the bandpass-filtered signal.
        int peak_1   = compute_peak (filtered, &peak_state_1);
        int thresh_1 = threshold (&threshold_state_1, peak_1);

        // Right-side processing
        // Fancy 5-point derivative of the bandpass-filtered signal.

        int deriv_2 = deriv_5pt (filtered, &deriv_5pt_state);
        if(max_deriv < deriv_2){
            max_deriv = deriv_2;
        }
        if( min_deriv> deriv_2){
            min_deriv = deriv_2;
        }

        // Save data points for plotting
        plot_point(i, sample, "raw");
        plot_point(i, filtered, "filtered");
        
        // Increment tick counter
        tick_count++;

        // Add debug prints
        printf("Sample %d: Raw=%u, Filtered=%d\n", i, sample, filtered);
    }

    fclose(input_file);

    // Generate gnuplot script
    FILE* script = fopen("plot_script.gnu", "w");
    fprintf(script, "set terminal png\n");
    fprintf(script, "set output 'ecg_analysis.png'\n");
    fprintf(script, "plot 'plot_data.txt' using 1:2 title 'Raw' with lines,");
    fprintf(script, "     'plot_data.txt' using 1:3 title 'Filtered' with lines\n");
    fclose(script);

    // Execute gnuplot
    // system("gnuplot plot_script.gnu");
    
    return 0;
}