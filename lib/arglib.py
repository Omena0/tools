import sys

args = sys.argv

usage = ''

_args = []

class ArgMatch: str|tuple[str]

class ParseResult: dict[str,str]

class Arg:
    def __init__(self,name:str|ArgMatch, match:ArgMatch=None):
        """Create a new argument.

        ### Match types:
        #### Switch: `--arg`
        The value of this argument will be the argument after it.

        If `match` is a tuple, the value will be the first one that matches. (mutually exclusive)

        #### Flag: `-arg`
        The value of this argument will be True if it is present. False otherwise.

        If `match` is a tuple, the value will be whicever matches. (mutually exclusive)

        #### Positional: `arg`
        The value of this argument will be the whatever is at the same position.

        If `match` is a tuple, the value will be the first argument that matches. (mutually exclusive)

        Args:
            name (str | ArgMatch): The argument name.
            match (ArgMatch, optional): The argument match, if it would be the same as name you can omit this argument.

        Raises:
            ValueError: Invalid argument match.
        """

        match = match or name
        self.name = name
        self.match = match
        self.value = None
        self.type  = None
        self.count = None

        if isinstance(match,str):
            self.count = 1
        elif isinstance(match,tuple):
            self.count = len(match)
            match = match[0]
        else:
            raise ValueError(f'Invalid argument match: {match}')

        if match.startswith('--'):
            self.type = 'switch'
        elif match.startswith('-'):
            self.type = 'flag'
        else:
            self.type = 'positional'
            self.pos = len(_args)

        _args.append(self)

    def _parse(self, args:list[str]) -> ParseResult:
        """Parse this argument.

        # THIS IS AN INTERNAL METHOD, DO NOT CALL!!

        use arglib.parse() instead.

        Args:
            args (list[str]): sys.argv

        Returns:
            False|ParseResult: False if the argument is invalid, ParseResult otherwise.
        """

        skip = 0
        for i,arg in enumerate(args[1:]):
            if skip:
                skip -= 1
                continue

            # Switch
            if arg.startswith('--') and self.type == 'switch':
                skip += 1
                # Match multiple
                if self.count > 1:
                    if arg in self.match:
                        self.value = {arg:args[i+1]}
                        break

                # Match single
                elif arg == self.match:
                    self.value = args[i+1]
                    break

            # Flag
            elif arg.startswith('-') and self.type == 'flag':
                # Match multiple
                if self.count > 1:
                    if arg in self.match:
                        self.value = True
                        break

                # Match single
                elif arg == self.match:
                    self.value = True
                    break

            # Positional
            elif self.type == 'positional':
                if self.count > 1:
                    if self.pos == i and arg in self.match:
                        self.value = arg
                        break

                if self.pos == i:
                    self.value = arg
                    break

            # Invalid
            else:
                return False

        if self.value:
            return {self.name: self.value}

def parse():
    parsed = {}
    for arg in _args:
        value = arg._parse(args)
        if value:
            parsed.update(value)

    if not parsed:
        return False

    return parsed