# 23 September 2026, 17:41 — the police-chase freeze is the USB driver, not the renderer

Player session on JSRF.app built from `b563762` (the ADX blocking-wait release;
`[BUILD]` line and `blocking-wait release installed` both present in the log).
It passed the combos test, the spot where the 17:13 session hung. The ADX guard
reported **0 stalls**, and its release **never fired**, so that hang did not
recur but the fix was not exercised either. The game then froze in the police
chase. The picture stopped changing at t=346 s; the process kept running at
444% CPU and kept presenting frames.

Log: `/private/tmp/jsrf-play-20260923-3/last-run.log`; `sample`:
`/private/tmp/jsrf-play-20260923-3/hang.sample`.

## What the evidence says

- **The main thread** is in `DrawIndexedVertices` -> `MakeRequestedSpace`
  (0x191530) -> `BlockOnTime` (0x191440) -> `KeWaitForSingleObject`, waiting
  for ring space. Inside that wait it pumps device interrupts
  (`bridge_device_irq_poll`) and is running a guest DPC in XPP, the input and
  USB driver: `sub_001C2E50` -> `sub_001C2AEB` -> `sub_001C29F7`. Both samples
  are inside it, so the DPC never returns and the thread never re-checks its
  fence.
- **The OHCI controller stopped at the same moment**, about t=345 s. Log line
  240,538 carries the session's only `tds_error=1`. `ien` goes from
  `80000073` to `00000073`, so the master interrupt enable was cleared (the
  guest's ISR does that; its DPC re-enables it on exit, which never happens).
  `tds_retired` freezes at 42,712 while `blocked` keeps rising.
- **It happened during rumble.** `out_reports` went from 267 to 283 across
  the window. The police chase is where the pad rumbles hardest, and the
  host-to-device data stage and XID rumble output report came in with
  `d9d3a6f`.

## Reading

Our OHCI model completed one transfer with an error condition, and XPP's
completion or recovery path in that DPC loops waiting for something the model
does not provide. That could be a halted endpoint cleared, a done-queue entry,
or a retry. **Not yet verified**: which TD errored, its condition code, and
what `sub_001C29F7` is looping on. Next steps:
- log the erroring TD (ED, direction, condition code, buffer pointers) in
  `xbox_usb_ohci.c`;
- read `sub_001C29F7`/`sub_001C2AEB` for the loop's exit condition;
- reproduce by driving rumble with a controller attached.

This is not the renderer and not the ADX guard. The flicker and the police
audio garbling reported in the same session are separate open items.
