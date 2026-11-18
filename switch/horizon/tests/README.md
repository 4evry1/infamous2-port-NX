# Horizon slab allocation regression

Run `python3 switch/horizon/tests/test_slab_allocation.py` from the checkout (host C compiler with ASan/UBSan required).

The runner extracts the current production slab transaction. Kernel adapters validate arguments and inject failures; the bounded heap models newlib's size-plus-alignment reservation. It verifies constrained-span success, exact zeroing, no publication/accounting after failure, rollback, retry and cap handling. This is host behavioral evidence, not proof of Horizon mapping or a particular game heap layout.

The regression must fail with the former 2 MiB backing alignment even though a page-aligned 2 MiB allocation fits. This mechanism was independently reproduced with devkitPro's actual newlib allocator in a bounded 3 MiB host heap and checked against the installed Switch libc disassembly. Hardware gameplay remains the acceptance gate for the freeze.
