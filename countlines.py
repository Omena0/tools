import sys
import os
import fnmatch
from itertools import zip_longest

def matches(path, gitignore: list):
    path = os.path.normpath(path)
    for pattern in gitignore:
        pattern = os.path.normpath(pattern)
        if pattern.startswith('/'):
            pattern = pattern[1:]
        if pattern.endswith('/'):
            pattern = pattern[:-1]
        if fnmatch.fnmatch(os.path.relpath(path, start=os.getcwd()), pattern) or fnmatch.fnmatch(os.path.basename(path), pattern):
            return True
    return False

def countlines(dir, ignore=['.*']): # By default ignore dotfiles
    if os.path.exists(os.path.join(dir,'.gitignore')):
        with open(os.path.join(dir,'.gitignore'), 'r') as f:
            for line in f.read().splitlines():
                if line and not line.startswith('#'):
                    ignore.append(f'{dir}/{line}')

    result = {}

    for thing in os.listdir(dir):
        path = os.path.normpath(os.path.join(dir, thing))

        if matches(path, ignore):
            continue

        if os.path.isdir(path):
            result.update(countlines(path, ignore))

        else:
            try:
                with open(path, 'r') as file:
                    lines = file.readlines()
                    result[path] = len(lines)
            except:
                continue

    return result

if __name__ == '__main__':
    if len(sys.argv) == 1:
        dir = '.'
    else:
        dir = sys.argv[1]

    lines = countlines(dir)
    lines = sorted(lines.items(), key=lambda x: x[1], reverse=True)
    max_length = len(lines[0][0]) if lines else 0
    total_lines = sum(count for _, count in lines)
    average_lines = total_lines / len(lines) if lines else 0
    min_length = len(lines[-1][0]) if lines else 0

    ### Everything else is just the tables lmfao
    ### Literally 50% of the code is just printing tables
    ### Console UI is hard ok...

    # Prepare per-extension table data
    extensions = {}
    for path, count in lines:
        ext = os.path.splitext(path)[1]
        extensions[ext] = extensions.get(ext, 0) + count

    extension_data = [f"{ext:{max_length}} ::: {count}" for ext, count in extensions.items()]

    # Prepare per-file table data
    file_data = [f"{path:{max_length}} ::: {count}" for path, count in lines]

    # Prepare lines of code table data
    lines_data = [
        f"Total: {total_lines}",
        f"Longest: {lines[0][1]}",
        f"Average: {average_lines:.2f}",
        f"Shortest: {lines[-1][1]}",
        f"Files: {len(lines)}",
    ]

    # Determine column widths based on header titles and data rows
    col1_width = max(len("Lines of code"), *(len(line) for line in lines_data))
    col2_width = max(len("Per extension"), *(len(line) for line in extension_data))
    col3_width = max(len("Per file"), *(len(line) for line in file_data))

    # Build and print header row
    header = (
        f"| {'Lines of code':<{col1_width}}  ||  "
        f"{'Per extension':<{col2_width}}  ||  "
        f"{'Per file':<{col3_width}} |"
    )

    # Header
    separator_line = (
        '+' + '=' * (col1_width + 3) + '++' +
              '=' * (col2_width + 4) + '++' +
              '=' * (col3_width + 3) + '+'
    )

    # Example output:
    # +=================++=========================++========================+
    # | Lines of code   ||  Per extension          ||  Per file              |
    # +=================++=========================++========================+
    # | Total: 368      ||  .py           ::: 327  ||  lib\arglib.py ::: 141 |
    # | Longest: 141    ||  .md           ::: 23   ||  countlines.py ::: 129 |
    # | Average: 74.00  ||  .cmd          ::: 20   ||  proj.py       ::: 57  |
    # | Shortest: 20    ||                         ||  README.md     ::: 23  |
    # | Files: 5        ||                         ||  build.cmd     ::: 20  |
    # +=================++=========================++========================+

    # Print header
    print()  # Spacer line
    print(separator_line)
    print(header)
    print(separator_line)

    # Print data
    for code_line, ext_line, file_line in zip_longest(lines_data, extension_data, file_data, fillvalue=""):
        print(
            f"| {code_line:<{col1_width}}  ||  "
            f"{ext_line:<{col2_width}}  ||  "
            f"{file_line:<{col3_width}} |"
        )

    print(separator_line)
    print()  # Spacer line

