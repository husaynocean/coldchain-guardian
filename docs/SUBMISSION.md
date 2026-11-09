# Submission kit (QNX Global Hackathon)

**Category:** Robotics & Medical
**Bonus prizes entered:** Best Codelab / Tutorial (`docs/CODELAB.md`) · Best use of QNX-specific Technologies
**Deadline:** 15 Dec 2026, 8:00 pm WAT

## Devpost project description (paste and edit)

**Coldchain Guardian: real-time vaccine protection that acts before the breach**

Vaccines are only safe between 2 °C and 8 °C. In northern Nigeria, clinic fridges sit in 32 °C+ heat and live through daily grid failures on generators and batteries. A monitor that reacts late doesn't just raise an alarm. It writes off a tray of vaccines nobody knows have lost potency.

Coldchain Guardian is the safety brain for that fridge, built on QNX 8.0:

- **Predicts.** A least-squares trend over the last 4 s of readings projects the seconds left until the vials leave 2–8 °C.
- **Acts first.** On grid loss, it engages battery cooling ~60 s *before* the projected breach. It also sounds the door buzzer and logs every decision.
- **Is never late, and shows it.** The guardian is a priority-60 thread woken every 5 ms by a kernel timer pulse. An identical loop runs at priority 10. Hit *Flood CPU* and the 3D dashboard (Three.js) shows the low-priority loop stall while the guardian keeps pace, drawn from measured tick counts and jitter.

**How it uses QNX (verifiable files):**

- `src/resmgr.c`: `/dev/coldchain` resource manager (`resmgr_attach`, `io_read`/`io_write`/`io_devctl`) with a custom `DCMD_CC_GET_SAMPLE` devctl. You can `cat` and `echo` it from the QNX shell.
- `src/guardian.c`: `timer_create` + `SIGEV_PULSE_INIT` + `MsgReceivePulse` 5 ms loop, with `ClockCycles()` jitter measurement and sensor reads via `devctl` (`MsgSend`, with priority inherited by the driver).
- `src/platform.c` / `src/main.c`: explicit `SCHED_FIFO`/`SCHED_RR` priorities, system page `qtime`, `trace_logf()` user events, `mlockall()`.
- `src/world.c`: `PTHREAD_PRIO_INHERIT` mutex.
- `tools/trace.sh`: `tracelogger`/`traceprinter` capture showing guardian events in the kernel trace.

**Results (fill in from your run):** on QNX 8.0 x86_64 under a full CPU flood, the guardian's worst-case wake-up jitter was ___ µs with ___ missed deadlines. Over the same window, the priority-10 loop reached ___ ms with ___ misses.

**Built with:** QNX SDP 8.0, C11, qcc, Three.js, Server-Sent Events.
**Target / toolchain:** QNX 8.0 x86_64 VM (mkqnximage), `qcc -Vgcc_ntox86_64`.

## Demo video script (≤ 2:50)

| time | on screen | voice-over |
|---|---|---|
| 0:00 | Dashboard, slow orbit, title | "This fridge holds 63 vials. Above 8 degrees, they start to die. Where I live, the grid fails every day." |
| 0:15 | Terminal: `uname -a` on target, `pidin -p coldchaind` | "This is QNX 8.0. The guardian is a priority-60 thread woken by a kernel timer every 5 milliseconds." |
| 0:35 | `cat /dev/coldchain`, then `echo power_off > /dev/coldchain` | "The sensor is a real device driver, a QNX resource manager. Watch what happens when I cut the power from the shell." |
| 0:50 | Fridge goes dark, ttb counts down, battery engages | "It doesn't wait for the alarm. It projects the breach and switches to battery cooling a minute early." |
| 1:20 | Press Flood CPU; outer orb stalls red, inner keeps going | "Now I flood every core. The same loop at normal priority starves. The guardian doesn't miss a beat. These are measured numbers, not animation." |
| 1:50 | Open the door by clicking it; cold air spills | "A nurse leaves the door open. It's still on time." |
| 2:10 | System Profiler / `trace.sh` output | "And here is the proof from the kernel's own trace log." |
| 2:30 | Proof strip close-up, then logo | "Coldchain Guardian. Built on QNX, so the vaccines don't depend on luck." |

Record at 1080p. Use your own voice and no copyrighted music. Upload to YouTube as public.

## 7-day plan

| day | goal | done when |
|---|---|---|
| 1 | QNX Everywhere licence, SDP 8.0, VM via `mkqnximage`, hello world on target | `hello` prints on target |
| 2 | `make qnx`, deploy, run `coldchaind`; fix any compile errors | dashboard shows **LIVE · QNX** |
| 3 | Run experiments, capture `trace.sh` + System Profiler screenshots, record numbers | results filled in above |
| 4 | Push repo to GitHub (public, new repo), add `docs/screenshot.png`, deploy `web/` to Vercel for the simulation preview | public URL works |
| 5 | Walk through CODELAB.md from scratch on a clean VM; fix every step that's wrong | a friend could follow it |
| 6 | Record + edit demo video | video < 3:00 on YouTube |
| 7 | Fill in the Devpost form, submit, post on X | submitted |

## Checklist

- [ ] New public repo created during the submission period, Apache-2.0 detected in About
- [ ] README has QNX version, target, toolchain, build + run steps
- [ ] Description names the QNX files and functions (above)
- [ ] Video < 3 min, public on YouTube, shows it running on QNX
- [ ] Category: Robotics & Medical
- [ ] Codelab URL in the "Best Codelab / Tutorial" field (link to `docs/CODELAB.md` on GitHub)
