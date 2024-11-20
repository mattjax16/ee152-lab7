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
struct biquadcoeffs {
    float b0, b1, b2, a0, a1, a2;
};

// Original biquad coefficients
static struct biquadcoeffs biquad_20Hz_lowpass[2] = {
    {8.59278969e-05f, 1.71855794e-04f, 8.59278969e-05f,
     1.0f, -1.77422345e+00f, 7.96197268e-01f},
    {1.0f, 2.0f, 1.0f,
     1.0f, -1.84565849e+00f, 9.11174670e-01f}
};

struct biquadstate { float x_nm1, x_nm2, y_nm1, y_nm2; };
static struct biquadstate biquad_state[2] = {0};

// Original biquad function
int biquad(const struct biquadcoeffs *coeffs, struct biquadstate *state,
           uint32_t sample, uint32_t n_bits) {
    float xn = ((float)sample) / ((float)(1<<n_bits));
    float yn = coeffs->b0*xn + coeffs->b1*state->x_nm1 + coeffs->b2*state->x_nm2
             - coeffs->a1*state->y_nm1 - coeffs->a2*state->y_nm2;

    state->x_nm2 = state->x_nm1;
    state->x_nm1 = xn;
    state->y_nm2 = state->y_nm1;
    state->y_nm1 = yn;

    return (yn * (1<<n_bits));
}

// Main desktop debug loop
int main() {
    input_file = fopen(ECG_FILE, "r");
    if (!input_file) {
        printf("Error: Cannot open input file\n");
        return 1;
    }

    // // Initialize plot file
    // FILE* gnuplot = fopen("ECG_FILE", "w");
    // fprintf(gnuplot, "# Time Raw Filtered Derivative ThresholdCrossing\n");
    // fclose(gnuplot);

    // Main processing loop
    for (int i = 0; i < 1000; i++) {  // Process 1000 samples for testing
        uint32_t sample = analogRead(0);
        
        // Apply biquad filter
        int filtered = sample;
        for (int j = 0; j < 2; j++) {
            filtered = biquad(&biquad_20Hz_lowpass[j], &biquad_state[j], filtered, 12);
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