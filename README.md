# LogiFn

> **Built 100% by Claude, Anthropic's AI.** Claude (in Claude Code) did the
> research, the protocol work, the code and the testing, entirely from a Mac.
> It reached a real Raspberry Pi 4 over the network with
> [Rorth](https://github.com/davidpsharp/Rorth), the RISC OS Remote Test
> Harness. No RISC OS emulator was used, and nobody sat at a RISC OS machine to
> develop it. The only hands-on part was testing: [David
> Sharp](https://github.com/davidpsharp) pressed keys and switched the
> keyboard off and on when asked (and power-cycled the Pi once, when an early
> probe froze it). The conversation that built it is in
> [docs/how-it-was-made.md](docs/how-it-was-made.md).

Source and releases: https://github.com/davidpsharp/LogiFn

A RISC OS 5 module that keeps a Logitech K400 Plus keyboard's top row in
function-key mode, so F1–F12 work without holding Fn. It also works with other
Logitech wireless keyboards that have an HID++ Fn-inversion feature.

Tested on a Raspberry Pi 4 with RISC OS 5.30 and a K400 Plus on a Unifying
receiver (046D:C52B).

## Use

Load it with `RMLoad LogiFn`, or put `LogiFn` in
`!Boot.Choices.Boot.PreDesk` to load it at every start-up. Nothing else is
needed. From then on, whenever a keyboard links up to a receiver (start-up,
power switch, waking from sleep, the receiver plugged in), LogiFn sets it to
function keys.

- `*LogiFn` shows the receivers and keyboards it has found and what it did.
- `*LogiFnMode Media` sets the keys back to the keyboard's media and shortcut
  actions; `*LogiFnMode FKeys` sets them to function keys again (the default).

Example, with two Unifying receivers, one of them the K400 Plus's (`404D`):

```
*LogiFn
LogiFn: F1-F12 set to function keys (FKeys)
USB7: receiver 046D:C52B, HID++ on interface 2, watching
  1: 404D linked, set, feature 40A2 at 9, set 1 time
USB5: receiver 046D:C52B, HID++ on interface 2, watching
  1: 4055 linked, not a keyboard
  2: 4076 not linked, offline
Recent events:
     17s ago  USB4.1 (404D): linked
     17s ago  USB4: watching receiver 046D:C52B
     17s ago  USB5: watching receiver 046D:C52B
     15s ago  USB7.1 (404D): linked
     15s ago  USB7: watching receiver 046D:C52B
     14s ago  USB7.1 (404D): feature 40A2 at 9
     14s ago  USB7.1 (404D): set to F-keys
      6s ago  USB5.1 (4055): linked
```

Each line under a receiver is a paired device: its slot, its wireless product
ID, whether it's linked now, and what LogiFn has done with it. Keyboards that
are asleep show as "not linked, offline" until a key is pressed.

## How it works

The keyboard has an HID++ 2.0 feature, *Fn inversion* (0x40A2 on the K400
Plus; 0x40A0 or 0x40A3 on some other keyboards), that swaps F1–F12 with the
media keys. The keyboard forgets the setting when it's switched off, which is
why setting it once from Logitech's software on another computer doesn't last.

The receiver has three USB interfaces. RISC OS's own drivers use the keyboard
(0) and mouse (1) ones. LogiFn opens the third, the HID++ interface, as a
DeviceFS stream (`devices#interface2;endpoint3;interrupt:USB<n>`) and sends
requests as HID SET_REPORT control transfers through `DeviceFS_CallDevice`.
It switches on the receiver's connection notifications (register 0x00) and
asks it to announce the devices already connected (register 0x02). Each time
a keyboard reports *linked* (notification 0x41), LogiFn looks up its Fn
feature through the root feature and sets it.

A 10 cs ticker queues a callback that does all the work; only one request is
outstanding per receiver at a time.

Only receivers with a separate HID++ interface (HID class 3/0/0) are used.
Receivers that put HID++ on the mouse interface, such as some nano receivers,
are left alone, because opening that interface would take the mouse away from
RISC OS.

## Building

`tools/build` builds `build/LogiFn,ffa` with GCCSDK in Docker (the
`riscosdotinfo/riscos-gccsdk-4.7` image). `tools/package` then makes
`dist/LogiFn-<version>.zip`, ready to unpack on RISC OS, with filetypes kept.

## Diagnostics

`tools/hidpp.bas` sends one raw HID++ request to a receiver and prints the
reply, or watches its notifications. See the comments at its top. Unload
LogiFn first.

## Licence

GNU General Public License, version 3 or later: see [LICENSE](LICENSE).
Copyright (C) 2026 [David Sharp](https://github.com/davidpsharp).
