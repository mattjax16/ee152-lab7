import re, numpy as np, matplotlib.pyplot as plt
#import pdb; pdb.set_trace()

# Globals used to build a data structure.
#   * signal_names[] is a list of the names from line #1 of the main input
#     file.
#   * values[] is a list of numpy arrays; one array for each signal. So
signal_names=[]		# List of the signals we traced, grabbed from line 1
                        # of the input file
values=None		# List of np arrays, one array per signal.

# Parse the input file.
# - Read 'filename' (which should be a file of dumped signal values from
#   lab7_host_main.cxx). The first line of 'filename' is a list of the signals
#   that were traced/dumped. Each of the remaining lines is a list of the
#   dumped values for those signals.
# - Build a data structure from reading 'filename'. The data structure is
#   described above.
# You shouldn't have to touch this routine.
# Parse the input file with CSV-style data
def parse_inputfile(filename):
    global signal_names, values
    infile = open(filename, "r")
    first = True
    for line in infile:
        # Split line by commas and remove surrounding whitespace
        fields = line.strip().split(",")
        if first:
            # The top line has the names of the signals we've dumped
            first = False
            signal_names = fields
            values = [[] for _ in fields]  # Create empty lists for each signal
            continue

        # Convert each value to an integer
        try:
            numbs = [int(f) for f in fields]
        except ValueError as e:
            print(f"Skipping invalid line: {line.strip()} ({e})")
            continue

        # Append numbers to corresponding signal lists
        for idx, num in enumerate(numbs):
            values[idx].append(num)

    # Convert the lists to numpy arrays
    for idx in range(len(values)):
        values[idx] = np.array(values[idx])


# Your routine to plot whatever signals you like.
def plot_what_you_want():
    print ("plotting...")
    # Plot whichever signals you want from what was read in.
    plot_signal ("Sample", 1, -2000)
    plot_signal ("Filtered", 1, -2000)
    #plot_signal ("Peak_1")
    #plot_signal ("Deriv_2",8)
    plot_signal("Dual_QRS", 1000)
    #plot_signal("Avg_200ms", 1, 0)
    #plot_signal("Left_QRS", 1000)
    #plot_signal("Right_QRS", 1000)
    #plot_signal("Left_Thresh", 1, 0)
    #plot_signal("Right_Thresh", 1, 0)

    # Plot a line for y=0 if desired. If not, then just comment this out.
    n_pts = values[0].size
    x_axis = np.arange (n_pts,dtype=float) * .002
    #plt.plot (x_axis, np.zeros_like(values[0]))

    plt.xlabel("Time (s)")
    plt.ylabel("Amplitude")
    plt.legend (loc="upper right")
    plt.show()

# Plot a signal by its name, and allow scaling and translation.
# So, plot signame*times + plus.
def plot_signal (signame, times=1, plus=0, spacing=.002):
    idx = signal_names.index(signame)
    n_pts = values[idx].size
    x_axis = np.arange (n_pts,dtype=float) * spacing
    data = values[idx]*times + plus
    plt.plot (x_axis, data, label=signame, marker=".")

parse_inputfile ("run.out")
plot_what_you_want()
