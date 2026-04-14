# smallsh
A POSIX-compliant shell implemented in C, built from scratch as part of an Operating Systems course at Oregon State University

## Overview

`smallsh` replicates core behaviors of a Unix shell — forking child processes, handling I/O redirection, managing background jobs, and responding to signals — using only POSIX system calls. No `system()`, no shortcuts.

## Features

- **Command execution** via `fork` / `execvp`
- **I/O redirection**: input (`<`), output (`>`), and append (`>>`)
- **Background execution** with `&`
- **Built-in commands**: `exit`, `cd`, `pwd`
- **Parameter expansion**: `$$` (PID), `$?` (last exit status), `$!` (last background PID), `${VAR}` (environment variable)
- **Signal handling**:
  - `SIGINT` (Ctrl-C): ignored in the shell parent; foreground children terminate normally
  - `SIGTSTP` (Ctrl-Z): ignored shell-wide; stopped background jobs are automatically continued
- **Background job reaping**: completed background processes are reaped and reported each prompt cycle

## Build

```bash
gcc -Wall -Wextra -std=c99 -o smallsh smallsh.c
```

## Usage

```bash
# Interactive mode
./smallsh

# Script mode
./smallsh script.sh
```

### Example session

```
$ ls > out.txt
$ cat out.txt
README.md
smallsh
smallsh.c
$ sleep 5 &
$ echo $$
12345
$ exit
```

## Implementation Notes

**Signal safety across fork boundaries**

The parent shell ignores `SIGINT` so Ctrl-C doesn't kill the shell itself, but foreground children need to receive it normally. Signal disposition is restored to `SIG_DFL` in the child immediately after `fork`, before `execvp`. Background children additionally keep `SIGTSTP` ignored to match standard shell behavior.

**Background job tracking**

Background PIDs are stored in a fixed-size array with a sentinel value. Each prompt cycle polls each tracked PID with `waitpid(..., WNOHANG)` to reap completed jobs without blocking — mimicking how production shells handle async job completion.

**Parameter expansion pipeline**

Expansion is handled by a two-pass scanner: `param_scan` locates the next `$` reference in a token, and `build_str` incrementally assembles the result string. This avoids in-place string mutation and handles multiple expansions per token correctly.

## License

MIT
