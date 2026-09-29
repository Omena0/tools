import sys
import os
import fnmatch
from itertools import zip_longest
from typing import Iterable

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

def countlines(dir, ignore=None, include=None) -> dict: # By default ignore dotfiles
    if ignore is None:
        ignore = ['.*']
    if include is None:
        include = []

    if os.path.exists(os.path.join(dir,'.gitignore')):
        with open(os.path.join(dir,'.gitignore'), 'r') as f:
            for line in f.read().splitlines():
                if not line:
                    continue
                if not line.startswith(('#','!')):
                    ignore.append(f'{dir}/{line}')
                elif line.startswith('!'):
                    include.append(f'{dir}/{line.removeprefix('!')}')

    result = {}

    for thing in os.listdir(dir):
        path = os.path.normpath(os.path.join(dir, thing))

        if matches(path, ignore) and not matches(path, include):
            continue

        if os.path.isdir(path):
            result |= countlines(path, ignore.copy(), include.copy())

        else:
            try:
                with open(path, 'r', encoding='utf-8') as file:
                    result[path] = sum(1 for _ in file)
            except:
                continue

    return result

def print_normalized(lines: Iterable[str], col_size=32):
    lines = list(lines)

    # Calculate col size
    cs = col_size
    for line in lines:
        # Add multiple of col_size if it doesnt fit in the gap
        while cs <= len(line):
            cs += col_size

    # Create the actual string
    text = ''
    for line in lines:
        new = line.ljust(cs)

        # Would this fit on the current line?
        if len(text.rsplit('\n', 1)[-1]) + len(new) > term_size[0]:
            text += '\n' # Doesnt fit. Next line.

        text += new

    print(text)

if __name__ == '__main__':
    dir = '.' if len(sys.argv) == 1 else sys.argv[1]
    limit = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith('-') else '500'
    if not limit.isnumeric():
        print(f'Usage: {sys.argv[0]} [directory] [limit]')
        exit(1)

    limit = int(limit)

    lines = countlines(dir)
    lines = list(sorted(lines.items(), key=lambda x: x[1], reverse=True))

    max_length = len(lines[0][0]) if lines else 0
    total_lines = sum(count for _, count in lines)
    average_lines = total_lines / len(lines) if lines else 0
    min_length = len(lines[-1][0]) if lines else 0

    ### Everything else is just the tables lmfao
    ### Literally 50% of the code is just printing tables
    ### Console UI is hard ok...

    term_size = os.get_terminal_size()

    # Prepare per-extension table data
    extensions = {}
    for path, count in lines:
        ext = os.path.splitext(path)[1]
        extensions[ext] = extensions.get(ext, 0) + count

    extensions = dict(sorted(extensions.items(), key=lambda x: x[1], reverse=True))

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

    fcount = len(lines)

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

    if '-short' in sys.argv[1:]:
        print(f'tot:{total_lines},max:{lines[0][1]},avg:{average_lines:.6f},min:{lines[-1][1]},fcount:{fcount}')
        print(','.join(f'{ext}:{lcount}' for ext, lcount in extensions.items()))
        print(','.join(f'{file}:{lcount}' for file, lcount in lines[:limit]))

        exit(0)

    if '-compact' in sys.argv[1:]:
        # Header
        print_normalized(
            [
                f'Total: {total_lines}',
                f'Max: {lines[0][1]}',
                f'Average: {average_lines:.2f}',
                f'Min: {lines[-1][1]}',
                f'Files: {fcount}'
            ]
        )

        # Per ext
        print('\nPer extension:')
        print_normalized(f'{ext}: {lcount:<16}' for ext, lcount in extensions.items())

        # Per file
        print('\nPer file:')
        print_normalized(f'{file}: {lcount}' for file, lcount in lines[:limit])

        exit(0)

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
    for i, (code_line, ext_line, file_line) in enumerate(zip_longest(lines_data, extension_data, file_data, fillvalue="")):
        if i >= limit:
            break

        print(
            f"| {code_line:<{col1_width}}  ||  "
            f"{ext_line:<{col2_width}}  ||  "
            f"{file_line:<{col3_width}} |"
        )

    print(separator_line)
    print()  # Spacer line

