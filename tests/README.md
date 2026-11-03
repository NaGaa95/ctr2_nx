Run `python3 tests/run_tests.py` with a host C compiler available as `cc`,
`clang`, or `gcc`. Set `CC` to choose a different host compiler.

The checks compile production configuration code. CTR1 also checks its actual
subscription tap handler with stubbed platform dimensions and purchase calls.
These tests do not run the Android engine or replace Switch gameplay tests.
