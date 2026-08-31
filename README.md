`ctsup` - Contract Supervisor
=============================

A tiny [illumos](https://illumos.org) contract supervisor.

Usage
-----

```
$ ctsup /path/to/services
```

```
$ ctsup -h
Usage: ctsup <dir>

Environment Variables
  CTSUP_QUIET       disable log messages from this program
  CTSUP_NO_COLOR    disables color output from this program
  NO_COLOR          same as above but may affect downstream services
```

Example
-------

Using the existing `services/`  directory included with this repo:

```
$ ls services/
bar   foo

$ ./ctsup ./services/
[2026-08-30T00:36:41.277Z] found 2 service files
[2026-08-30T00:36:41.278Z] foo: started pid 3330 contract 3264
[2026-08-30T00:36:41.278Z] bar: started pid 3331 contract 3265
foo: hello from 3330
bar: hello from 3331
foo: exiting
[2026-08-30T00:36:44.286Z] foo: contract 3264 empty
[2026-08-30T00:36:44.286Z] foo: restarting in 1 second
[2026-08-30T00:36:45.287Z] foo: started pid 3351 contract 3266
foo: hello from 3351
^C
[2026-08-30T00:36:46.934Z] shutting down.
[2026-08-30T00:36:46.934Z] sending SIGTERM to all contracts
[2026-08-30T00:36:46.936Z] bar: contract 3265 empty
[2026-08-30T00:36:46.936Z] foo: contract 3266 empty
```

Development
-----------

Ensure your code passes the style checks:

```
$ make check
./tools/check ./src/*.c ./tools/* *.md Makefile
checking:  ./src/main.c
checking:  ./tools/check
checking:  README.md
checking:  Makefile
```

License
-------

MIT License
