# How LogiFn was made: a Claude Code session

This is the conversation from the Claude Code session that produced LogiFn,
from the first request to a working, tested module. **The module was built
100% by Claude** (Anthropic's AI, running in Claude Code on a Mac). Claude did
the research, protocol experiments, coding, building and testing itself. It
reached a real Raspberry Pi 4 over the network with
[Rorth](https://github.com/davidpsharp/Rorth), the RISC OS Remote Test Harness.
No RISC OS emulator was used, and nobody sat at a RISC OS machine to develop it.
The only hands-on part was testing: David pressed keys and switched the
keyboard off and on when asked, and power-cycled the Pi once after an early
probe froze it.

Between these messages, Claude ran a few hundred commands on its own: reading
the RISC OS USB driver's source and Solaar's, probing the receiver from BASIC
on the Pi, building with GCCSDK in Docker, installing and testing. Those, and
Claude's short progress notes, are left out; this is just the conversation.
Times are minutes from the first message.

## Timeline

| Time | What happened |
| --- | --- |
| 0 min | The request. |
| 3–7 min | Research: the HID++ Fn-inversion protocol (from Solaar and an old Linux tool), and how to reach the receiver's HID++ interface on RISC OS (from the USBDriver source). |
| 8–14 min | A first probe freezes the Pi; David power-cycles it. |
| 14–21 min | Step-by-step probes find the K400 Plus, its feature (0x40A2 at index 9), and set F-keys. Switching the keyboard off resets it. |
| 21–28 min | The receiver's connection notifications show the keyboard reconnecting. The freeze is re-run deliberately and doesn't recur. |
| 28–33 min | The module is written, built and loaded. |
| 33–35 min | It works: a key press, then a power cycle, and the mode is set each time. |
| 35–39 min | More tests, tidying up, README. |

---

### 👤 David (0 min)

> I have a logitech K400+ keyboard that I use regularly with RISC OS 5.30 on a Raspberry Pi 4 (and other Pis). An annoying feature of the keyboard is that it needs to have the Fn key held down to access the function keys which are a key part of a lot of RISC OS functionality. There is a logitech options software for Windows/mac/linux that allows you to modify that but it doesn't stay that way when the keyboard goes back to RISC OS. The goal is to have a way to make the keyboard normally operate in function key mode on RISC OS 5, likely with a small relocatable module or similar.

**🤖 Claude:** Found the key protocol detail: the K400+ responds to a HID++ 2.0 message (`10 01 09 19 00 00 00`, feature index 9 = FN_INVERSION 0x40A0, value 0 = standard F-keys). Now I'm looking up how to send that over the RISC OS 5 USB stack.

**🤖 Claude:** Good news: your Rorth harness lets me reach RISC OS machines directly, and you have a GCC module toolchain in Docker. Reading how they work.

**🤖 Claude:** Both are Unifying receivers (C52B) with a separate HID++ interface (2), so a module can talk to them without disturbing the keyboard/mouse drivers. I'll write a quick BASIC probe first to confirm the protocol on the real Pi 4 before building the module.

### 👤 David (9 min, while Claude was working)

> what's happening?

**🤖 Claude:** I've hung your Pi 4. It needs a power cycle. Sorry, especially since someone had been using it about a minute before I took the lease.

**Where things stand:**

1. **Protocol: found.** The K400+ supports HID++ 2.0 feature `0x40A0` (Fn inversion). Sending `setGlobalFnState(0)` through the Unifying receiver makes F1–F12 send real function keys. The existing Linux tool [lenisko/k400_plus](https://github.com/lenisko/k400_plus) sends exactly this (`10 01 09 19 00 …`), and Solaar's source agrees. The setting is lost when the keyboard reconnects or powers off, which is why Logitech Options' change doesn't stick. So the module has to re-apply it each time the keyboard reconnects.
2. **RISC OS route: found.** I read the RISC OS USBDriver source. The receiver's HID++ channel is on interface 2, which RISC OS's built-in keyboard and mouse drivers don't claim. So a module can open it through DeviceFS and send commands with `DeviceFS_CallDevice` (USB Control Request) without upsetting the keyboard. Your Pi 4 has two Unifying receivers (USB5 and USB8), both laid out that way.
3. **Live test: froze the machine.** My BASIC probe ran under Rorth. The first run hit an error (a string-terminator bug of mine) and left two USB streams open. I closed those and ran the fixed probe on USB5. The Pi then stopped answering, even to ping, and Rorth's watchdog didn't bring it back.

**Likely cause:** a blocking USB read or the synchronous control request locked up the whole machine. A DeviceFS read that waits for data stalls everything, even inside a TaskWindow. I'll make the next probe safer: send-only first, reads only of bytes already sitting in the buffer, and a timeout on the stream.

**What I need from you:**
- Power-cycle the Pi 4 when it suits you, and tell me if anything on screen looked odd before it froze.
- Should I keep testing on the Pi 4, or use the Pi 3 or Pi 2? Testing there needs a K400+ receiver plugged in, since the emulators have no USB. If the K400+ is your everyday keyboard on the Pi 4, its receiver could be moved to one of the others while I experiment.

My lease on pi4 will lapse by itself. Once I can confirm the command works, I'll write the module in C with your GCCSDK Docker image. It will look for the keyboard on any Logitech receiver and set function-key mode at startup and again whenever the keyboard reconnects.

### 👤 David (10 min)

> can you use rorth to reboot it or is it locked up?

**🤖 Claude:** No, Rorth can't reboot it. It's fully locked up: I just checked again and it doesn't answer ping, never mind Rorth. `ro-run reboot` and `reboot --hard` both work by asking the Rorth server on the Pi to reset the machine, so they only work while the Pi still answers on the network. Rorth's own watchdog, which resets the machine when a command overruns, didn't fire either. That suggests the hang is low down, probably in the USB stack with interrupts off, so nothing on the Pi gets to run.

It needs a power cycle at the machine: unplug it and plug it back in. Once it's back up I'll check it with `ro-run info`.

### 👤 David (14 min)

> it's back

**🤖 Claude:** It's back, and the machine is answering. I've checked the USB driver source: the synchronous control request busy-waits for at most 5 seconds, so it shouldn't hang forever on its own. I don't yet know which step froze the Pi. So I'll run the probe one step at a time, each step writing to a log file on the SD card first, so if it hangs again we'll know where.

**🤖 Claude:** Reading works, and the reply came back: `10 01 8F 00 0A 09 00`. That's a HID++ error (0x09) from receiver slot 1, which usually means nothing is connected in that slot right now, or the device is asleep. So the earlier hang came from the multi-slot loop, not from basic sending and reading.

**🤖 Claude:** USB4 slot 1 is the K400 Plus (wireless ID `40 4D`). USB5 has two other Logitech devices (IDs 4055 and 4076).

**🤖 Claude:** I've found which receiver has the K400+, but it's asleep, so I can't talk to it yet. **Please press a key on it** (Shift is harmless) or move a finger on its touchpad. It should be switched on.

**What I've found since the reboot** (all on the Pi 4, without hanging it again):
- Opening the receiver's HID++ channel, sending a request and reading the reply all work.
- The K400+ (wireless ID 404D) is in slot 1 of receiver **USB4**. Receiver USB5 holds two other Logitech devices.
- When I ask the keyboard for the index of its Fn-inversion feature, the receiver answers with error 0x09. That usually means the keyboard is asleep and out of reach.

**What's still unexplained:** the first freeze happened while one run sent several requests back to back, to slots 1–6. For now every run sends one request and the loop is on the Mac, so a run can't repeat that pattern. Before I build the module, I'll work out what caused the freeze.

A background loop is sending the request once per run for about the next two minutes. As soon as the keyboard wakes and answers, the loop stops, and I'll send the command that switches it to function-key mode, so you can try F1–F12 straight away.

### 👤 David (17 min, while Claude was working)

> i've pressed keys

**🤖 Claude:** Thanks, that worked: the keyboard is answering now.

**🤖 Claude:** The full reply arrived this time (earlier I just read too soon). It says feature 0x40A0 is index 0, which means the K400+ doesn't support it. Trying the two newer Fn-inversion features, 0x40A2 and 0x40A3:

**🤖 Claude:** The K400+ uses **feature 0x40A2 at index 9**, which matches the Linux tool.

**🤖 Claude:** Current state 1 (media keys), factory default 1. Now setting it to 0 (standard F1–F12):

**🤖 Claude:** I've switched the K400+ on your Pi 4 into function-key mode. The keyboard confirms the change: its Fn state now reads 0 (standard F1–F12) instead of 1 (media keys). **Please try the F-keys without holding Fn**, for example F12 for the command line, or F3 to save in an editor.

**How it's done.** The K400+ uses HID++ feature 0x40A2 at index 9; I'd assumed 0x40A0 at first, but the keyboard doesn't have that one. The commands go through receiver USB4, the keyboard's own receiver, and the keyboard is in slot 1:
- read state: `10 01 09 0A 00 00 00`
- set standard F-keys: `10 01 09 1A 00 00 00`

These match the Linux tool's commands. A RISC OS module can find all of this automatically: which receiver, which slot, which feature index.

**One more test that would help:** after you've tried the keys, switch the keyboard off with its power switch, wait a few seconds, switch it back on, and try F12 again. That shows whether the setting survives a power cycle. If it doesn't, the module will need to notice reconnections and set the mode again; I expect it won't survive.

I still need to find what froze the Pi the first time. Every single-request run since has been fine. I'll test that before the module does any back-to-back requests.

### 👤 David (21 min)

> i turned it off for about 10 seconds and on again and it didn't keep the functionality

**🤖 Claude:** That's expected: the keyboard forgets the setting at power-off. So the module has to notice when the keyboard reconnects and set the mode again.

The receiver can report connect and disconnect events over the HID++ channel, but only after "wireless notifications" are switched on in its register 0x00. I'm adding a "watch" mode to the probe: it turns those notifications on, then logs every packet for 90 seconds.

**🤖 Claude:** The watcher is running now, for the next 90 seconds. **Please switch the K400+ off, wait about 5 seconds, then switch it back on.** If the receiver sends a "device connected" packet when the keyboard comes back, the module can use it to set function-key mode again straight away.

### 👤 David (22 min)

> done

**🤖 Claude:** That's the event the module needs. The receiver reported both changes:

| Time | Packet | Meaning |
|---|---|---|
| 18.1 s | `10 01 41 04 61 4D 40` | slot 1 (wireless ID 404D, the K400+) **link lost** (bit 0x40 set) |
| 30.8 s | `10 01 41 04 A1 4D 40` | slot 1 **link established** |
| 30.8 s | `11 01 04 00 01 01 01 …` | the keyboard's own "reconnected, needs reconfiguring" event (feature index 4) |

So the module can wait for "link established" from a K400+ (or the keyboard's own event) and then send the set-F-keys command, with no polling.

Before I write the module, I want to understand the earlier freeze. The module will send several requests in a row while it looks for keyboards, and that's roughly what was running when the Pi froze.

> **Claude asked:** Can I deliberately re-run the sequence that froze the Pi 4 (6 requests in a row to the other receiver, USB5), now logging every step to the SD card? It may freeze it again, needing another power cycle from you, but it would tell me what to avoid in the module.
>
> **David chose:** Yes, try it now

**🤖 Claude:** No freeze this time: all six requests went through and the stream closed cleanly. One detail: slot 1's answer came back late, about a second after its request, so the module must match replies to requests and not assume they arrive in order.

**🤖 Claude:** No freeze again, even after the leaked stream. I think I've found the real cause anyway, and it was my mistake. The very first probe started with a stray line, `SYS "OS_ServiceCall",,&D2,0`. That broadcasts Service_USB "new device attached" to every USB client module, with a null pointer where the device block should be. That first run also crashed ("the command never returned to Rorth"). Whatever read through that null pointer probably left the system corrupted, and the next run then froze it. I removed the line before the later runs, and nothing has gone wrong since.

**🤖 Claude:** I have what I need. Writing the module, called **LogiFn**. In outline:
- It finds Logitech receivers that have a separate HID++ interface (class 3/0/0, as on your two C52B receivers). It also checks newly attached devices.
- For each receiver, it opens the HID++ stream, switches on connect/disconnect notifications, and asks the receiver to announce the devices already connected.
- When a keyboard links up, it finds the Fn-inversion feature (0x40A2, then 0x40A0, then 0x40A3) and sets F-key mode.
- A 10 cs ticker queues a callback that does all the work. At most one request per receiver is in flight, with timeouts and retries.
- `*LogiFn` shows the status and a short event log. `*LogiFnMode FKeys|Media` chooses the mode.

**🤖 Claude:** It built cleanly, with no warnings. Loading it on the Pi 4 and checking its status:

**🤖 Claude:** Working so far. Both receivers are being watched. The K400+ (404D) is reported "not linked", probably because it's asleep. Slot 2 on USB5 (4076) is a second keyboard, also asleep. **Could you press a key on the K400+?** That shows whether waking it from sleep makes the receiver report it as linked, or whether the module will need another way to notice.

### 👤 David (33 min)

> key pressed

**🤖 Claude:** It works. Your key press woke the keyboard, the receiver reported "linked", and within a second the module had found feature 0x40A2 and set F-key mode.

Two checks, if you would:
1. **Try F1–F12 without Fn.**
2. **Switch the K400+ off and on again**, then press a key and try the F-keys again. The module should set the mode again automatically this time.

### 👤 David (34 min)

> tested and seems to work great

**🤖 Claude:** The log confirms it. After you switched the keyboard off ("link lost") and on again ("linked"), the module set F-keys again within a second.

Two more tests before I tidy up: unloading and reloading the module, and resetting the K400+'s receiver (`*USBReset`), which exercises the same unplug/replug handling as pulling the dongle out. **Your keyboard and mouse may drop out for a second or two during the reset.**

**🤖 Claude:** That passed too. When the receiver was reset it came back as USB7. The module dropped USB4 when that device died, picked up USB7, and set F-keys again. Unloading and reloading also worked cleanly.

**🤖 Claude:** The module is working on your Pi 4. It found the K400+ on its receiver, sets F1–F12 to function keys whenever the keyboard links up, and you confirmed the keys work. It's loaded from `$.code.K400.LogiFn`, but only until the next restart: nothing in your boot sequence loads it yet.

**Tested on the Pi 4:**
- Loading it, and waking the keyboard from sleep: the mode is set within about a second.
- Switching the keyboard off and on: the mode is set again.
- Resetting the receiver: it came back as USB7 and the mode was set again.
- Unloading and reloading the module.

**Not tested:**
- Your other Pis.
- Keyboards that use the other two Fn-inversion features (0x40A0, 0x40A3). The 0x40A3 command format is taken from another project's source, not checked on a keyboard.
- Receivers other than the Unifying C52B. Receivers that share the HID++ channel with the mouse are deliberately left alone, because opening it would take the mouse away from RISC OS.

**The earlier freeze:** I think it was my fault. The first probe broadcast a USB "device attached" service call with a null pointer, and nothing has gone wrong since that line was removed. I couldn't make it happen again with the same request sequence, so I can't be certain. I've deleted that probe.

**One side effect:** releasing the Rorth lease "put back" system variables that the USB driver had created during my receiver reset. So `DeviceFS$USB7$Options` is now gone, and stale `USB4` variables are back. It's very likely harmless, and the next restart or replug fixes it. Rorth shouldn't count variables the system sets as the lease holder's, so I'd like to file an issue on Rorth for it, if you're happy with that.

**In the project:**
- `src/logifn.c` and `src/ModHdr.cmhg`: the module.
- `tools/build`: builds `build/LogiFn,ffa` in your GCCSDK Docker image.
- `tools/hidpp.bas`: a diagnostic tool that sends one raw request to a receiver or watches its notifications.
- `README.md`.

Nothing is committed yet.

Should I:
1. Put LogiFn in `!Boot.Choices.Boot.PreDesk` on the Pi 4 (and the Pi 3 and Pi 2) so it loads at every start-up?
2. Commit the project to git?
3. File the Rorth issue?

### 👤 David (39 min)

> yes please on all 3, file it under a GPL v3 license please and include the example *logifn command example from [a file on my NAS] if it's helpful to do so as an example. include a link to my github in the package
