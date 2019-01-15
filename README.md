# out-err
A command line tool to capture stdout and stderr simultaneously

Usage: `out+err [-o FILE] COMMAND [ARG]...`

Run `COMMAND`, combining chunks sent to `STDOUT` and `STDERR` into a single
file, preserving the relative order.  Every chunk starts with a
4 byte header.  A header encodes the data size as a 32 bit big endian
number.  Only 31 lower bits are used.  The high bit is 0 for `STDOUT`,
1 for `STDERR`.
