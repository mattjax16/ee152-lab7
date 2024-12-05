#include <iostream>
#include <sstream>
#include <fstream>
#include <stdlib.h>
#include "stdint.h"
#include <cstdio>
#include <vector>

// My own function for printing -- feel free to remove it.
using namespace std;
#define LOG(args) cout << args << endl
#define DIE(args) { cout << args << endl; exit(0); }

// A replacement for analogRead(); pass it the name of the file to get
// data from.
static int analogRead(string filename) {
    static ifstream in_file;
    static bool first_time = 1;
    if (first_time) {
	first_time = 0;
	in_file.open (filename);
	if (!in_file.is_open())
            DIE ("Cannot open "<<filename);
    }

    // Each time we're called, read one line from the input file. Each line
    // should just have one number on it (followed by an optional comma); we
    // return the number.
    string line;
    if (!getline(in_file, line))
	return (-1);
    istringstream iss(line);
    int val;
    if (! (iss >> val))
	return (-1);
    return (val);
}

#include <stdbool.h>

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

// Structure for storing signal peaks
struct PeakDetection {
    int start_idx; // Q point (index of lowest point before peak)
    int peak_idx;  // R point (index of the peak itself)
    int end_idx;   // S point (index of lowest point after peak)
};

// Global vectors to store signals and QRS detections
vector<int> samples;
//vector<int> filtered_values;
vector<int> dual_qrs_values;

#define REFRACTORY_TICKS 100 // 200 ms at 500 Hz
const int THRESHOLD = 2500;            // Threshold for detecting peaks

int main() {
    // Open output file
    ofstream out_file("run.out");
    if (!out_file.is_open()) {
        DIE("Cannot open output file run.out");
    }

    // Write header line for signals
    out_file << "Sample,Filtered,Dual_QRS\n";

    //int prev_filtered = 0;
    int sample_count = 0; 
    //int refractory_counter = 0; 
    bool detecting_peak = false;
    PeakDetection current_peak = {-1, -1, -1};
    int last_sample = 0;
    //struct compute_peak_state peak_state_1, peak_state_2;

    for (int n_its=0; n_its<2; ++n_its) {
        for (;;) {
            uint32_t sample = analogRead("phaidra_formatted.txt");
            if (sample == -1) break;
            
            //uint32_t scaled_sample = sample * 2;

            // Process signal
            int filtered = sample;
            samples.push_back(sample);
            dual_qrs_values.push_back(0);

            // // Joel's stuff
            for (int i = 0; i < N_BIQUAD_SECS; ++i)
                filtered = biquad(&biquad_20Hz_lowpass[i], &biquad_state[i], filtered, 12);

            // int peak_1 = compute_peak(filtered, &peak_state_1);
            // int thresh_1 = threshold(&threshold_state_1, peak_1);

            // int deriv_2 = deriv_5pt(filtered, &deriv_5pt_state);
            // int deriv_sq_2 = deriv_2 * deriv_2;

            // int avg_200ms_2 = window_ravg(deriv_sq_2);
            // int peak_2 = compute_peak(avg_200ms_2, &peak_state_2);
            
            // if (++sample_count < 250) continue;
            // int thresh_2 = threshold(&threshold_state_2, peak_2);

            // int left_thresh = thresh_1;
            // int right_thresh = thresh_2;

            // dual_QRS_last = dual_QRS;
            // ++refractory_counter;

            //  // Calculate slopes
            // int filtered_slope = filtered - prev_filtered;
            // int avg_200ms_slope = avg_200ms_2 - prev_avg_200ms;

            // // Update previous values
            
            // prev_avg_200ms = avg_200ms_2;

            // //dual_QRS = (filtered > left_thresh) && (avg_200ms_2 > right_thresh) && (refractory_counter > REFRACTORY_TICKS);
            // int refractory_OK = (refractory_counter > REFRACTORY_TICKS);
            //     // left_QRS = refractory_OK && (filtered > left_thresh),
            //     // right_QRS = refractory_OK && (avg_200ms_2 > right_thresh);
            // int left_QRS = (filtered_slope > 0);
            // int right_QRS = (avg_200ms_2 > right_thresh);
            // dual_QRS = left_QRS && right_QRS;

            // prev_filtered = filtered;

            // // if (dual_QRS_last && !dual_QRS) {
            // //     refractory_counter = 0;
            // // }

                // out_file << sample << "," << scaled_sample << "," << filtered << ","
                //         << avg_200ms_2 << "," << left_thresh << "," << right_thresh << ","
                //         << dual_QRS << "," << left_QRS << "," << right_QRS << "\n";
            // Peak detection logic
            // Peak detection logic


            // QRS detection logic based on raw sample and threshold
            if (sample > THRESHOLD) {
                if (!detecting_peak) {
                    // Start of a new QRS detection
                    detecting_peak = true;
                    current_peak.start_idx = sample_count; // Start at the first point above threshold
                    
                    for (int i = sample_count - 1; i >= 0; --i) {
                        if (samples[i] < samples[current_peak.start_idx]) {
                            current_peak.start_idx = i;
                        } 
                        else {
                            break; // Stop when the signal stops decreasing
                        }
                    }
                    current_peak.peak_idx = sample_count;  // R point (initially same as start)
                } else {
                    // Update the peak during detection
                    if (sample > samples[current_peak.peak_idx]) {
                        current_peak.peak_idx = sample_count; // Update R point
                    }
                }
            } else {
                if (detecting_peak) {
                    // End of a QRS detection
                    detecting_peak = false;
                    current_peak.end_idx = sample_count; // End at the first point below threshold

                    for (int i = sample_count; i < samples.size(); ++i) {
                        if (samples[i] < samples[current_peak.end_idx]) {
                            current_peak.end_idx = i;
                        }
                        else {
                            break; // Stop when the signal stops decreasing
                        }
                    }

                    // Set dual_qrs to 1 for all indices from Q to S
                    for (int i = current_peak.start_idx; i <= current_peak.end_idx; i++) {
                        if (i >= 0 && i < dual_qrs_values.size()) {
                            dual_qrs_values[i] = 1;
                        }
                    }
                    
                    // Output detected QRS region
                    LOG("Detected QRS: Q=" << current_peak.start_idx
                                      << ", R=" << current_peak.peak_idx
                                      << ", S=" << current_peak.end_idx);

                    // Write data to file for the QRS region
                    for (int i = current_peak.start_idx; i <= current_peak.end_idx; i++) {
                        out_file << samples[i] << "," << filtered << "," << dual_qrs_values[i] << "\n";
                    }

                    // Reset peak detection
                    current_peak = {-1, -1, -1};
                }
            }   

            // Log data to file outside of QRS region
            if (!detecting_peak) {
                out_file << sample << "," << filtered << "," << dual_qrs_values[sample_count] << "\n";
            }

            last_sample = sample;
            ++sample_count;
        }
    }
    
    // Close file and print final log
    out_file.close();
    LOG("Finished processing samples. Results saved to run.out.");

    return 0;
}


// #include <iostream>
// #include <sstream>
// #include <fstream>
// #include <stdlib.h>
// #include "stdint.h"

// // My own function for printing -- feel free to remove it.
// using namespace std;
// #define LOG(args) cout << args << endl
// #define DIE(args) { cout << args << endl; exit(0); }

// // A replacement for analogRead(); pass it the name of the file to get
// // data from.
// static int analogRead(string filename) {
//     static ifstream in_file;
//     static bool first_time = 1;
//     if (first_time) {
// 	first_time = 0;
// 	in_file.open (filename);
// 	if (!in_file.is_open())
//             DIE ("Cannot open "<<filename);
//     }

//     // Each time we're called, read one line from the input file. Each line
//     // should just have one number on it (followed by an optional comma); we
//     // return the number.
//     string line;
//     if (!getline(in_file, line))
// 	return (-1);
//     istringstream iss(line);
//     int val;
//     if (! (iss >> val))
// 	return (-1);
//     return (val);
// }

// #include <stdbool.h>

// // Dual_QRS indicates that both the left & right side of the algorithm believe
// // we have a QRS, and that we're not in the refractory period.
// // Dual_QRS_last is just a one-cycle-delayed version of dual_QRS; it's used to
// // detect the rising edge of dual_QRS.
// static bool dual_QRS = false;
// static bool dual_QRS_last = false;

// //****************************************************
// // Biquad filtering.
// //****************************************************

// struct biquadcoeffs {	// The coefficients of a single biquad section.
//     float b0, b1, b2,	// numerator
// 	  a0, a1, a2;	// denominator
// };
// // Our 20Hz lowpass filter is built from two biquad sections.
// #define N_BIQUAD_SECS 2	// Number of biquad sections in our filter.
// static struct biquadcoeffs biquad_20Hz_lowpass[2] = {
// 	{8.59278969e-05f, 1.71855794e-04f, 8.59278969e-05f,
// 	 1.0f,		-1.77422345e+00f, 7.96197268e-01f},
// 	{1.0f,	2.0f,	1.0f,
// 	 1.0f,	-1.84565849e+00f,	9.11174670e-01f}};

// // All DSP filters need state.
// struct biquadstate { float x_nm1, x_nm2, y_nm1, y_nm2; };
// static struct biquadstate biquad_state[N_BIQUAD_SECS] = {0};

// // Biquad filtering routine.
// // - The input is assumed to be a 12-bit unsigned integer coming straight from
// //   the ADC. We convert it immediately to a float xn in the range [0,1).
// // - Compute yn = b0*xn + b1*x_nm1 + b2*x_nm2 - a1*y_nm1 - a2*y_nm2
// // - Update x_nm1->x_nm2, xn->x_nm1, y_nm1->y_nm2, yn->y_nm1
// // - Return yn as a 12-bit integer.
// int biquad (const struct biquadcoeffs *coeffs, struct biquadstate *state,
// 	    uint32_t sample, uint32_t n_bits) {
//     float xn = ((float)sample) / ((float)(1<<n_bits)); // current sample
//     float yn = coeffs->b0*xn + coeffs->b1*state->x_nm1 + coeffs->b2*state->x_nm2
// 	     - coeffs->a1*state->y_nm1 - coeffs->a2*state->y_nm2; // output

//     state->x_nm2 = state->x_nm1;
//     state->x_nm1 = xn;
//     state->y_nm2 = state->y_nm1;
//     state->y_nm1 = yn;

//     return (yn * (1<<n_bits));
// }

// //****************************************************
// // Calculate a derivative with a fancy five-point algorithm.
// //****************************************************

// // Compute derivative using a 5-point algorithm.
// // Given an input 'sample', keep track of its last 5 values, which they call
// // xp2, xp1, x0, xm1 and xm2. Then when we get a new sample, compute & return
// //	(-xm2 - 2*xm1 + 2*xp1 + xp2)/8
// // and then shift to do xm2=xm1, xm1=x0, x0=xp1, xp1=xp2, xp2=sample
// // So you can think of this as implementing a differentiator with a delay of
// // two time units.
// struct deriv_5pt_state {
//     int xp2, xp1, x0, xm1, xm2;
// };

// struct deriv_5pt_state deriv_5pt_state = { 0 };
// struct deriv_5pt_state deriv_5pt_state1 = { 0 };
// struct deriv_5pt_state deriv_5pt_state2 = { 0 };

// int deriv_5pt (int sample, struct deriv_5pt_state *state) {
//     int r = -state->xm2 - 2*state->xm1 + 2*state->xp1 + state->xp2;
//     r = r>>3;	// Divide by 8.

//     state->xm2 = state->xm1;
//     state->xm1 = state->x0;
//     state->x0 = state->xp1;
//     state->xp1 = state->xp2;
//     state->xp2 = sample;

//     return (r);
// }

// //****************************************************
// // Windowing algorithm
// //****************************************************

// // Just a running average of the last 100 samples, using a circular buffer.
// // Used by the right-side algorithm.
// #define WINDOW_SIZE 100 // samples for the running average.
// static int window_ravg (int sample) {
//     static int window_buf [WINDOW_SIZE] = { 0 };
//     static int window_ptr = 0;
//     static long window_sum = 0;

//     window_sum -= window_buf[window_ptr];
//     window_buf[window_ptr] = sample;
//     window_sum += sample;
//     window_ptr = (window_ptr+1) % WINDOW_SIZE;

//     return (window_sum / WINDOW_SIZE);
// }

// //****************************************************
// // The moving-threshold algorithm, used as the near-final stage of the left-side
// // and right-side calculations.
// //****************************************************

// struct threshold_state {
//     int threshold; // running peak threshold
//     int max, min;
//     int decay; // amount that max and min thresholds decay each sample
// };
// struct threshold_state threshold_state_1 = { 0x7FF, 0x000, 0xFFF, 15 };
// struct threshold_state threshold_state_2 = { 0x7FF, 0x000, 0x2FF, 4 };

// // The moving-threshold algorithm.
// // It always keeps a running min & running max. Each time we get a new sample
// // (and hence call this function)...
// //    - A negative sample is the exception; immediately return the current
// //	threshold (which is (min+max)/2).
// //    - The new sample goes into the running min & max.
// //    -	The max decrements by a fixed delta, and the min increments by the
// //	same fixed delta. Clamp the max to never go <0, and the min to never
// //	go >0xFFF.
// //    - Return (min + max)/2
// // Does it really make sense to have max < min??? This algorithm allows that!
// // And note that this algorithm is completely different than Pan Tompkins.
// int threshold( struct threshold_state * state, int psample ) {
//     if (psample <= 0) return (state->threshold); // no sample peak

//     if (psample > state->max) state->max = psample;
//     if (psample < state->min) state->min = psample;

//     // Implement decay.
//     state->max -= state->decay;
//     if (state->max < 0x000) state->max = 0x000;
//     state->min += state->decay;
//     if (state->min > 0xFFF) state->min = 0xFFF;

//     state->threshold = (state->min + state->max)/2;
//     return (state->threshold);
// }

// struct compute_peak_state {
//     struct deriv_5pt_state der5_state;
//     int prev_deriv;
// };

// // Usually return 0; but when the input signal hits a peak, then return the
// // value of the signal (i.e., of the peak).
// int compute_peak (int sample, struct compute_peak_state *state) {
//     // First compute the derivative.
//     int deriv = deriv_5pt (sample, &state->der5_state);

//     // Peak==1 if the current sample fell and the previous one rose. I.e., we
//     // just had a peak.
//     bool peak = (state->prev_deriv>=0) && (deriv<0);
//     state->prev_deriv = deriv; // update derivative history

//     // We'll usually return 0, but data if we just had a peak.
//     return (peak? sample : 0);
// }

// #define REFRACTORY_TICKS 100 // 200 ms at 500 Hz
// int main() {
//     // Open output file
//     ofstream out_file("run.out");
//     if (!out_file.is_open()) {
//         DIE("Cannot open output file run.out");
//     }

//     // Write header line for signals
//     out_file << "sample filtered peak_1 deriv_2 deriv_sq_2 dual_QRS\n";

//     LOG("sample\tfiltered\tpeak_1\tderiv_2\tderiv_sq_2\tdual_QRS");

//     int sample_count = 0; // To ignore startup artifacts.
//     int refractory_counter = 0; // Refractory counter for QRS detection
//     struct compute_peak_state peak_state_1, peak_state_2;

//     for (;;) {
//         // Read ADC sample
//         uint32_t sample = analogRead("ecg_normal_board_calm1.txt");
//         if (sample == -1) break; // End of file
//         sample *= 2; // Example scaling, if needed

//         // Process signal through biquad filters
//         int filtered = sample;
//         for (int i = 0; i < N_BIQUAD_SECS; ++i)
//             filtered = biquad(&biquad_20Hz_lowpass[i], &biquad_state[i], filtered, 12);

//         // Left-side analysis (Peak and threshold)
//         int peak_1 = compute_peak(filtered, &peak_state_1);
//         int thresh_1 = threshold(&threshold_state_1, peak_1);

//         // Right-side processing (Derivative and squared derivative)
//         int deriv_2 = deriv_5pt(filtered, &deriv_5pt_state);
//         int deriv_sq_2 = deriv_2 * deriv_2;

//         // Running average over a 200 ms window
//         int avg_200ms_2 = window_ravg(deriv_sq_2);

//         // Right-side analysis (Peak and threshold)
//         int peak_2 = compute_peak(avg_200ms_2, &peak_state_2);
//         if (++sample_count < 250) continue;
//         int thresh_2 = threshold(&threshold_state_2, peak_2);

//         // QRS detection
//         dual_QRS_last = dual_QRS; // Pipe stage for edge detect
//         ++refractory_counter;
//         dual_QRS = (filtered > thresh_1) && (avg_200ms_2 > thresh_2) &&
//                    (refractory_counter > REFRACTORY_TICKS);
//         if (dual_QRS_last && !dual_QRS) refractory_counter = 0;

//         // Log to console
//         LOG(sample << "\t" << filtered << "\t" << peak_1 << "\t" << deriv_2 << "\t"
//                    << deriv_sq_2 << "\t" << dual_QRS);

//         // Write to output file
//         out_file << sample << " " << filtered << " " << peak_1 << " " << deriv_2 << " "
//                  << deriv_sq_2 << " " << dual_QRS << "\n";
//     }

//     // Close the output file
//     out_file.close();
// }
