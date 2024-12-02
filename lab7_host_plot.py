import matplotlib.pyplot as plt

# Initialize lists to store the data
time = []
raw = []
filtered = []

# Read the data from the file
with open('plot_data.txt', 'r') as file:
    for line in file:
        parts = line.split()
        time.append(int(parts[0]))
        if parts[2] == 'raw':
            raw.append(int(parts[1]))
        elif parts[2] == 'filtered':
            filtered.append(int(parts[1]))

# Plot the data
plt.figure(figsize=(10, 6))
plt.plot(time[:len(raw)], raw, label='Raw', color='blue')
plt.plot(time[:len(filtered)], filtered, label='Filtered', color='red')
plt.xlabel('Time')
plt.ylabel('Value')
plt.title('ECG Data')
plt.legend()
plt.grid(True)
plt.show()