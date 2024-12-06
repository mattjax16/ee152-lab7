import os

input_filename = "matt_EKG.txt"
output_filename = "matt_formatted.csv"  # Ensure the output is .csv

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



# def reformat_numbers(input_file, output_file):
#     # Open the input and output files
#     with open(input_file, 'r') as infile, open(output_file, 'w') as outfile:
#         # Read all numbers into a single string
#         data = infile.read().replace("\n", "").split(",")
        
#         # Write each number to a new line with a trailing comma
#         for num in data:
#             num = num.strip()  # Remove any extra spaces
#             if num:  # Ignore empty lines
#                 outfile.write(f"{num},\n")

# # Input and output file names
# input_filename = "ecg_normal_board_calm1.txt"  # Replace with your input file name
# output_filename = "reformatted_numbers.txt"  # Replace with your output file name

# # Call the function
# reformat_numbers(input_filename, output_filename)
# print("Reformatted numbers saved to:", output_filename)
