/*
 * Connections:
 *	Drive a canned ECG onto Nano A3, if you don't want to use your own ECG.
 *	Reads an analog signal from Nano A0 using the ADC.
 *	Writes the ADC output to UART #2 (which drives USB to the host), and
 *		to Nano A4 via a DAC for debug purposes. 
 *
 * Usage:
 *	At startup, types "Type the letter 'g' to go"
 *	When they do that, it turns on the green LED and starts sampling.
 *	Typical sample speed is 500 samples/second.
 *	While it is sampling, it's also writing the digital output to USB at
 *	9600 baud.
 *	When sampling is done, the LED changes from solid green to blink at 1Hz.
 *	When printing is done, the LED turns off.
 */

#define N_DATA_SAMPLES 5000	// Take this many samples.
#define SAMPLE_DELAY 2		// Sample every 2 ms (so, 500 samples/sec).

// Include FreeRTOS headers.
#include "FreeRTOSConfig.h"
#include "FreeRTOS.h"
#include "portmacro.h"
#include "task.h"
#include "timers.h"
#include "stm32l4xx.h"
#include <stdbool.h>
#include "stm32l432xx.h"
#include "lib_ee152.h"

static uint16_t g_data[N_DATA_SAMPLES];
static bool g_data_collection_on = 0;
static int g_n_samples_taken=0;
static bool g_printing_done = 0;

#define TICKS_PER_CANNED_ECG_PT 2	// Assume it was sampled at 500 Hz.

//-----------------------------------------
// Drive a canned ECG onto Nano A3, if you don't want to use your own ECG.
//-----------------------------------------
#define ECG_DATA_FILE "ecg_normal_board_calm1_redone.c_data"
static unsigned short int ECG_data[] = {
#include ECG_DATA_FILE
};
void task_canned_ECG (void * pvParameters) {
    int n_datapoints = (sizeof ECG_data) / (sizeof (short int));

    int i=0;
    while (1) {
	if (++i == n_datapoints)
	    i = 0;
	unsigned int data = ECG_data[i];
	analogWrite (A3, data>>4);
	vTaskDelay(TICKS_PER_CANNED_ECG_PT);
    }
}

//-----------------------------------------
// Blink the green LED to show status:
//	Blink when collecting data
//	Steady on when done collecting data but still printing it
//	Off otherwise
//-----------------------------------------
#define BLINK_GREEN_DELAY ( 500 / portTICK_PERIOD_MS )
void task_blink_green(void *pvParameters) {
    // The green LED is at Nano D13, or PB3.
    pinMode(D13, "OUTPUT");
    int on=0;

    for ( ;; ) {
	if (g_data_collection_on)
	    on = 1;
	if (!g_data_collection_on && !g_printing_done)
	    on = !on;
	if (g_printing_done)
	    on = 0;
	digitalWrite (D13, on);
	vTaskDelay(BLINK_GREEN_DELAY);
    }
}

//-----------------------------------------
// Read analog data from Nano A0. Send it out again via DAC2 on Nano A4.
// And add it to the queue (in g_data[]), of samples to print via UART.
//-----------------------------------------
void task_ADC (void * pvParameters) {
    while (1) {
	uint32_t sample = analogRead(A0);	// Nano A0 = PA0
	analogWrite (A4, sample>>4);		// 12-bit ADC, 8-bit DAC

	if (g_data_collection_on)
	    g_data[g_n_samples_taken++] = sample;
	g_data_collection_on &= (g_n_samples_taken < N_DATA_SAMPLES);
	vTaskDelay(SAMPLE_DELAY);
    }
}

#define MAX_DIGITS 6
static char *int_to_string (int val) {
    static char buf[MAX_DIGITS+1];

    int pos = MAX_DIGITS;	// rightmost position.
    buf [MAX_DIGITS] = '\0';

    while ((val>0) && (--pos >= 0)) {
	int digit = val % 10;
	val /= 10;
	buf[pos] = digit + '0';
    }
    if (pos==MAX_DIGITS)	// Special case: val=0 yields empty string
	buf[--pos]='0';
    return (&buf[pos]);
}

void task_UART_write (void * pvParameters) {
    // Wait for a character to be typed before starting official data collection
    serial_write (USART2, "Type the letter 'g' to go\r\n");
    while (serial_read(USART2) != 'g')
	;
    g_data_collection_on = 1;	// Tells the ADC to start filling our array.

    int n_chars_printed = 0;
    while (1) {
	if (n_chars_printed < g_n_samples_taken) {
	    int sample = g_data[n_chars_printed];
	    serial_write (USART2, int_to_string (sample));
	    serial_write (USART2, "\n\r");
	    ++n_chars_printed;
	} else if (n_chars_printed==N_DATA_SAMPLES)
	    g_printing_done = 1;
    }
}

int main(void){
    clock_setup_80MHz();		// 80 MHz, AHB and APH1/2 prescale=1x
    serial_begin (USART2);

    // Create tasks.
    TaskHandle_t task_handle_green = NULL;
    BaseType_t status = xTaskCreate (
	    task_blink_green, "Blink Green LED",
	    100, // stack size in words
	    NULL, // parameter passed into task, e.g. "(void *) 1"
	    tskIDLE_PRIORITY+2, // priority
	    &task_handle_green);
    if (status != pdPASS)
	for ( ;; );

    TaskHandle_t task_handle_UART = NULL;
    status = xTaskCreate (
	    task_UART_write, "Write data to the UART",
	    100, // stack size in words
	    NULL, // parameter passed into task, e.g. "(void *) 1"
	    tskIDLE_PRIORITY+1, // priority
	    &task_handle_UART);
    if (status != pdPASS)
	for ( ;; );

    TaskHandle_t task_handle_ADC = NULL;
    status = xTaskCreate (
	    task_ADC, "Take ADC samples",
	    100, // stack size in words
	    NULL, // parameter passed into task, e.g. "(void *) 1"
	    tskIDLE_PRIORITY+3, // priority
	    &task_handle_ADC);
    if (status != pdPASS)
	for ( ;; );

    TaskHandle_t task_handle_canned_ECG = NULL;
    status = xTaskCreate (
	task_canned_ECG, "Task to drive a canned ECG out to PA3",
	100, // stack size in words
	NULL, // parameter passed into task, e.g. "(void *) 1"
	tskIDLE_PRIORITY+2, // priority
	&task_handle_canned_ECG);
    if (status != pdPASS )
	for ( ;; );

    vTaskStartScheduler();
}
