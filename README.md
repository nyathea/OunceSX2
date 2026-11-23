![logo](https://github.com/nyathea/OunceSX2/blob/b514f103e64881cea512089f8d4f8218e299e1b7/switch/app/icon.jpg)

# OunceSX2 — Native ARM64 JIT Fork of PCSX2 for Nintendo Switch 2

OunceSX2 is a free and open-source PlayStation 2 (PS2) emulator based on PCSX2. Its purpose is to emulate the PS2's hardware, using a combination of MIPS CPU [Interpreters](<https://en.wikipedia.org/wiki/Interpreter_(computing)>), [Recompilers](https://en.wikipedia.org/wiki/Dynamic_recompilation) and a [Virtual Machine](https://en.wikipedia.org/wiki/Virtual_machine) which manages hardware states and PS2 system memory.

This fork adds support for the Switch 2 (Ounce) platform.

## Known issues:
Some games may still crash at some points and its being looked into. other games may have slowdown issues. please leave Performance Mode enabled for heavier titles.

## Thank You

The ARMSX2 team is eternally indebted to the [PCSX2 project](https://pcsx2.net) it is based on. We are so fortunate to build on their 20 years of hardcore development.

The upstream PCSX2 project ships an ARM64 *interpreter* build for ARM, but its high-performance **JIT recompilers** (EE, IOP, VU0, VU1, and vtlb fast memory) are x86-64 only. 

Thanks to [@PalindromicBreadLoaf](https://github.com/PalindromicBreadLoaf) for the SW1 version which this is based on. and [@Rodpad](https://github.com/Rodpad) for SW2PS2 which improved on performance on the Switch 2.
