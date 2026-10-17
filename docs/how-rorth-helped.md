# How Rorth made LogiFn quick to build

LogiFn was built 100% by Claude, Anthropic's AI, working in Claude Code on a
Mac. [Rorth](https://github.com/davidpsharp/Rorth), the RISC OS Remote Test
Harness, is what let it try things on the real Raspberry Pi 4 and see the
results itself, in seconds, without a person relaying them. This page
explains, in Claude's words, what that made possible and how it would have
gone without it. The conversation itself is in
[how-it-was-made.md](how-it-was-made.md).

## What could be done without the Pi

Claude didn't need Rorth for the desk work:

- finding the protocol in [Solaar](https://github.com/pwr-Solaar/Solaar)'s
  source and an old Linux tool,
  [lenisko/k400_plus](https://github.com/lenisko/k400_plus);
- reading the RISC OS USBDriver and DeviceFS source to work out how to reach
  the receiver;
- writing the module, and building it with GCCSDK in Docker.

## What only the real machine showed

- **The K400 Plus uses feature 0x40A2, not 0x40A0.** The Linux tool only
  hardcodes an index (9) and never names the feature. Every source pointed at
  0x40A0, so a module written from them would have looked for that one first.
- **What the receiver does when the keyboard wakes or is switched on.** It
  sends a "linked" notification, but only after notifications are switched on
  in its register 0x00. The whole design of the module rests on that.
- **How RISC OS's USB stack actually behaves:**
  - replies can arrive late, up to a second after the request, or in pieces;
  - devices get new numbers (USB4, USB7, ...) after a reset or a restart;
  - reads are only safe if they take bytes already in the buffer;
  - DeviceFS doesn't close a stream when its device goes away, so the module
    has to.
- **A freeze.** Claude's first probe broadcast a USB service call with a null
  pointer and hung the machine. If a mistake like that had first appeared in
  an untested module installed in `!Boot.Choices.Boot.PreDesk`, it could have
  hung the Pi at every start-up.

Without testing, the result would have been something plausible, with a fair
chance that it didn't work and a real risk that it froze the machine.

## The other ways to investigate

From most to least useful:

1. **The receiver on the Mac.** Plugged into the Mac, the HID++ side could
   have been tested from Python with hidapi: the feature lookup, setting the
   mode, the reconnection notifications. That covers the keyboard's
   behaviour, but none of the RISC OS USB behaviour.
2. **A shared folder as a relay.** Claude writes test programs to a network
   share, a person runs them on the Pi, and they write logs back for Claude to
   read. It works, but each step needs the person. They'd have been running
   Claude's probes for most of the session, including the one that froze the
   Pi.
3. **A person as Claude's hands.** They type commands and paste the output
   back. That's the slowest way, and the easiest to get wrong, especially for
   things that depend on timing, like replies arriving a second late or a key
   press during a 90-second watch.
4. **An emulator.** RPCEmu and the other RISC OS emulators don't pass USB
   devices through to RISC OS, so they're no help with anything involving the
   receiver. They're only useful for checking that a module loads and its
   commands work.

## What Rorth changed

With Rorth, Claude ran a few hundred commands on the Pi from the Mac:
copying files, running BASIC probes and `*` commands, loading the module,
reading its status, restarting machines, and installing it on three Pis. The
person's part came down to:

- pressing keys on the keyboard,
- switching the keyboard off and on, and
- one power cycle, after the freeze.

Without Rorth, the same work would have been dozens of round trips. It would
have taken much longer than the 39 minutes from first request to finished,
tested module, and the person would have needed to follow the details of the
testing.
