set terminal png
set output 'ecg_analysis.png'
plot 'plot_data.txt' using 1:2 title 'Raw' with lines,     'plot_data.txt' using 1:3 title 'Filtered' with lines
