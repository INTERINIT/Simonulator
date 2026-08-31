# Notices and licensing

Simonulator is an out-of-tree driver overlay for [MAME](https://www.mamedev.org/).
It is not affiliated with MAMEdev, IBM, Mitsubishi Electric, Cirrus Logic, or
any cellular operator.  “MAME” is a registered trademark of Gregory Ember.

The overlay was developed and tested against MAME 0.289, commit
`fbda0e727c853b7af6f4d2c3ebf297b807787900`.  MAME is GPL-2.0; see
`licenses/GPL-2.0.txt`.  `src/mame/ibm/simon.cpp` retains its own
BSD-3-Clause licence and copyright notice.  The layout source declares
CC0-1.0 in its header.

IBM Simon BIOS and Flash images are **not included**.  They may be protected
by copyright and are needed to run the driver.  Users must dump firmware from
hardware they are authorised to use and must comply with applicable law.

No claim is made that this virtual network interoperates with an AMPS network.
It is a local, high-level test switch designed to exercise the Simon firmware.
