import os

input_filename = "phaidra.txt"
output_filename = "phaidra_formatted.csv"  # Ensure the output is .csv

# Check if file exists before proceeding
if not os.path.exists(input_filename):
    print(f"Error: {input_filename} not found in {os.getcwd()}.")
    exit(1)

# Function to read numbers from the input file
def read_numbers_from_file(filename):
    with open(filename, 'r') as file:
        numbers = []
        for line in file:
            # Split and convert each number to an integer
            numbers.extend(map(int, line.split()))
        return numbers

# Function to write numbers to a CSV file with trailing commas
def write_numbers_to_csv_with_trailing_commas(numbers, output_filename, line_length=10):
    with open(output_filename, 'w') as file:
        for i in range(0, len(numbers), line_length):
            # Slice the numbers for the current line
            line = numbers[i:i + line_length]
            # Join the numbers with commas, add a trailing comma, and a newline
            file.write(",".join(map(str, line)) + ",\n")

# Read numbers from the input file
numbers = read_numbers_from_file(input_filename)

# Write numbers to the CSV output file with trailing commas
write_numbers_to_csv_with_trailing_commas(numbers, output_filename)

print(f"Formatted numbers with trailing commas have been written to {output_filename}.")
