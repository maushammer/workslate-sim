# Workslate Emulator

An emulator for the 1983-era Workslate computer from Convergent Technologies.  Try out this vintage spreadsheet-only computer!

**[Try it in the browser here.](https://maushammer.github.io/workslate.html)**

## Requirements
This code can be run with two different interfaces:
- Runs in a browser with a graphical keyboard and the correct LCD fonts.  Requires the [Cheerp Web Assembly compiler](https://cheerp.io).
- A terminal-based experience with ANSI art and a full debugger. Stop the code, trace it, set breakpoints, etc.  Requires just gcc.  Should be portable, but only tested on OSX.


## Building:
```
make web      -- makes webasm version only
make term     -- makes version to run in terminal
make all      -- makes all
make clean    -- cleans the output directories
```

## Running
Both versions require access to shared resource files in order to run.

The web version assembles all need files in the `build/webpage` output directory.  It can be served locally via a web browser, such as `python3 -m http.server`.  Access it at [http://localhost:8000/workslate.html](http://localhost:8000/workslate.html)

The terminal version requires the `resource` directory to be in the current working directory.  For example, from the project directory, run `build/gcc/workslate`

## Copyright

- Incorporates processor simulation code from [EXORsim](https://github.com/jhallen/exorsim) M6800 Simulator Copyright &copy; 2011 Joseph H. Allen (GPL license).
- Modifications for 6801 Copyright &copy; 2020 Joel Matthew Rees [(exorsim6801)](https://osdn.net/users/reiisi/pf/exorsim6801/scm/commits/92ef8a291a6240887b19ec9117ea774aba59ea9a)
- Modifications for 6803 & WorkSlate emulation Copyright &copy; 2025 John Maushammer.

See LICENSE file for more details.
