import os

input_filename = "phaidra.txt"

# Check if file exists before proceeding
if not os.path.exists(input_filename):
    print(f"Error: {input_filename} not found in {os.getcwd()}.")
    exit(1)

# Proceed with file operations
def read_numbers_from_file(filename):
    with open(filename, 'r') as file:
        numbers = []
        for line in file:
            numbers.extend(map(int, line.split()))
        return numbers

def write_numbers_to_file(numbers, output_filename, line_length=10):
    with open(output_filename, 'w') as file:
        for i in range(0, len(numbers), line_length):
            line = ",".join(map(str, numbers[i:i+line_length]))
            file.write(line + '\n')

output_filename = "phaidra_formatted.txt"

numbers = read_numbers_from_file(input_filename)
write_numbers_to_file(numbers, output_filename)
print(f"Formatted numbers have been written to {output_filename}.")
