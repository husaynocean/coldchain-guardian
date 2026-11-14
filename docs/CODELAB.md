# Codelab: From web developer to real-time: build a vaccine-fridge guardian on QNX 8.0

**Duration:** about 2 hours · **Level:** beginner to QNX, comfortable with C and a terminal · **Cost:** free (QNX Everywhere)

I'm a frontend developer. Before this project, "real-time" to me meant WebSockets. This codelab is the guide I wish I'd had. By the end, a C program you wrote runs on the same OS that ships in 275 million cars. It exposes a device driver, keeps a 5 ms deadline under heavy load, and streams its proof to a 3D dashboard in your browser.

### What you'll build

A miniature **Coldchain Guardian** that:

1. Publishes a fridge sensor as a **device file** `/dev/coldchain` (a QNX *resource manager*)
2. Reads it from a **priority-60 control loop** woken by a **kernel timer pulse** every 5 ms
3. Measures its own timing with `ClockCycles()`, then survives a **CPU flood** that starves an identical low-priority loop
4. Leaves **kernel-level evidence** with `trace_logf()` + `tracelogger`
5. Streams everything to a **Three.js** dashboard

### What you'll learn

- How QNX is built: a microkernel where drivers are ordinary processes and **everything is a message**
- Resource managers: `resmgr_attach`, `io_read`, `io_write`, `io_devctl`
- Pulses and timers: `ChannelCreate`, `SIGEV_PULSE_INIT`, `timer_create`, `MsgReceivePulse`
- Priorities, `SCHED_FIFO` vs `SCHED_RR`, and **priority inheritance** through message passing
- How to *prove* timing claims with the QNX instrumented kernel

### What you need

- A Windows, Linux or macOS computer with about 15 GB free and 8 GB of RAM (macOS: build on a Linux VM)
- VirtualBox or VMware (Windows), or QEMU/KVM (Ubuntu)
- The repo: `git clone https://github.com/husaynocean/coldchain-guardian`

> 🪟 **Windows users, read this first.** I built this on a Windows 11 laptop, and every command below was run in **Command Prompt (cmd)**, not PowerShell. The QNX environment script is a `.bat` file: it only works in cmd, and only for the window you run it in. If you see `'qcc' is not recognized`, you're in PowerShell or a fresh window. Type `cmd`, then run `qnxsdp-env.bat` again.

---

## 1. Get QNX SDP 8.0 for free (30 min, plus waiting for the licence)

1. Create a **myQNX account** at qnx.com and click the activation link in the email.
2. While signed in, request the free non-commercial licence at **qnx.com/getqnx**. It can take a day or more to arrive. If it hasn't appeared after 24 hours, email the address on that page; mine arrived after I did.
3. When it shows up in the **myQNX License Manager** (qnx.com/account/dashboard), click **Accept License**, then click the **Users** cell → **Deploy**, and enter your own email.
4. Download **QNX Software Center** for your OS from the QNX download page and sign in with the same account.
5. Choose **Add Installation**, expand **QNX® Software Development Platform 8.0**, select the SDP itself (not a BSP or add-on), and install to the default folder: `~/qnx800` on Linux, `C:\Users\<you>\qnx800` on Windows. It's several GB.
6. Load the environment in every new terminal:

```bash
source ~/qnx800/qnxsdp-env.sh             # Linux
```
```bat
C:\Users\<you>\qnx800\qnxsdp-env.bat     :: Windows, in cmd.exe
```

✅ **Checkpoint:** `qcc -V` lists `gcc_ntox86_64` and `gcc_ntoaarch64le`, and `where mkqnximage` (Windows) or `which mkqnximage` (Linux) finds the image builder.

> 💡 **Web-dev translation:** `qcc` is to QNX what `tsc` + `webpack` are to a browser. It's a cross-compiler. You build on your laptop and ship the binary to the target.

## 2. Boot a QNX target with no hardware (20 min)

`mkqnximage` generates a complete bootable QNX system as a virtual machine. On Windows, QEMU isn't supported, so use VirtualBox (or VMware).

**VirtualBox, one-time setup:** open **File → Tools → Network Manager → Host-only Networks**. If the list is empty, click **Create**, and make sure **DHCP Server** is enabled. The default `192.168.56.1/24` is fine.

**Make an SSH key first.** The image only allows `root` to log in over SSH with a key, not a password:

```bat
ssh-keygen -t ed25519            :: press Enter three times
```

**Fix the SSH server before the first build.** Newer QNX OpenSSH packages split `sshd` into helper programs (`sshd-session`, `sshd-auth`) that the default image doesn't include, so `sshd` fails at boot with `/usr/libexec/sshd-auth does not exist`. Check that your SDP has them, then add two snippet files (this follows the workaround in the QNX 8.0.2 networking release notes):

```bat
mkdir C:\qnxvm
cd C:\qnxvm
dir C:\Users\<you>\qnx800\target\qnx\x86_64\usr\libexec
mkdir local\snippets
echo procmgr_symlink /system/bin/sshd-session /usr/libexec/sshd-session> local\snippets\ifs_env.custom
echo procmgr_symlink /system/bin/sshd-auth /usr/libexec/sshd-auth>> local\snippets\ifs_env.custom
echo bin/sshd-session=usr/libexec/sshd-session> local\snippets\system_files.custom
echo bin/sshd-auth=usr/libexec/sshd-auth>> local\snippets\system_files.custom
```

Leave out any line for a helper that `dir` didn't list. On Linux, write the same lines into the same files with any editor.

**Build and boot:**

```bat
mkqnximage --type=vbox --arch=x86_64 --ssh-ident=C:/Users/<you>/.ssh/id_ed25519.pub --run
mkqnximage --getip
```

Use `--type=qemu` on Ubuntu, and `~/.ssh/id_ed25519.pub` for the key path. `--run=-h` starts the VM without a window.

**Log in.** Type the IP with no `< >` around it (cmd treats those as special characters):

```bat
ssh root@192.168.56.104
```

If Windows' built-in ssh client fails with `Corrupted MAC on input`, force a different cipher. You'll need these two options on every `ssh` and `scp` to this VM:

```bat
ssh -c aes128-ctr -o MACs=hmac-sha2-256 root@192.168.56.104
```

✅ **Checkpoint:** `uname -a` on the target prints `QNX ... 8.0.0 ... x86_64`.

> ⚠️ **Before restarting Windows,** run `mkqnximage --stop`. Otherwise VirtualBox saves the VM's state, and if any host setting changes in between, it refuses to restore it. If that happens: `"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" discardstate noname`.

## 3. Hello, target (5 min)

```c
// hello.c
#include <stdio.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>
int main(void) {
    printf("hello from QNX, %u CPUs, %llu cycles/s\n",
           _syspage_ptr->num_cpu,
           (unsigned long long)SYSPAGE_ENTRY(qtime)->cycles_per_sec);
}
```

```bash
qcc -Vgcc_ntox86_64 -o hello hello.c
scp hello root@<ip>:/tmp/ && ssh root@<ip> /tmp/hello
```

The *system page* is a read-only structure the kernel maps into every process. It is QNX's "window.navigator".

## 4. Your first driver: a resource manager (25 min)

On Linux, a driver lives in the kernel. On QNX it's **a normal process** that claims part of the path namespace. When any program calls `open("/dev/coldchain")`, the kernel turns it into a *message* to your process.

The skeleton has four parts. The full version is in `src/resmgr.c`.

```c
dpp = dispatch_create();                                    // 1. a channel to receive messages
iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs,    // 2. default POSIX behaviour…
                 _RESMGR_IO_NFUNCS, &io_funcs);
io_funcs.read   = io_read;                                  //    …overridden where we care
io_funcs.write  = io_write;
io_funcs.devctl = io_devctl;
iofunc_attr_init(&attr, S_IFCHR | 0666, NULL, NULL);
resmgr_attach(dpp, &rattr, "/dev/coldchain", _FTYPE_ANY, 0, // 3. claim the path
              &connect_funcs, &io_funcs, &attr);
ctp = dispatch_context_alloc(dpp);
for (;;) { ctp = dispatch_block(ctp); dispatch_handler(ctp); } // 4. receive → handle → reply
```

`io_write` turns `echo power_off > /dev/coldchain` into a command:

```c
nbytes = _IO_WRITE_GET_NBYTES(msg);
resmgr_msgget(ctp, buf, take, sizeof(msg->i));   // copy the payload out of the client
cc_command(buf, NULL, 0);
_IO_SET_WRITE_NBYTES(ctp, nbytes);               // what the client's write() returns
return _RESMGR_NPARTS(0);
```

Build and run the full daemon now. We'll explore the rest piece by piece.

```bat
:: Windows (cmd): compiles, copies and runs in one step. Ctrl+C stops it on QNX too.
cd C:\dev\coldchain-guardian
deploy.bat 192.168.56.104
```
```bash
# Linux
make qnx && TARGET=192.168.56.104 ./tools/deploy.sh
ssh root@192.168.56.104 /data/coldchain/coldchaind --web /data/coldchain/web
```

Open `http://<ip>:8080` in your browser. The pill in the corner should read **LIVE · QNX 8.0.0**.

> 🐛 **The bug I hit here:** the first run printed `open /dev/coldchain: No such file or directory`, even though the driver had just announced itself. QNX hides a resource manager's own paths from its own process, to stop a single-threaded server from deadlocking on itself. The guardian lives in the same process as the driver, so it couldn't see it. The fix is one flag: `resmgr_attach(..., _RESMGR_FLAG_SELF, ...)`. It's safe here because the driver runs its own dispatch thread.

✅ **Checkpoint:** on the target,

```sh
ls -l /dev/coldchain
cat /dev/coldchain            # temp=4.61 door=0 power=1 ...
echo door_open > /dev/coldchain; sleep 5; cat /dev/coldchain   # temperature climbing
echo door_close > /dev/coldchain
```

You just talked to a driver you wrote, with `cat` and `echo`.

## 5. A binary API with devctl (10 min)

Text is nice for humans, but a control loop wants a struct, in one round trip:

```c
#define DCMD_CC_GET_SAMPLE  __DIOF(_DCMD_MISC, 0x31, cc_sample_t)   // "F" = data flows From driver

// client (guardian):
devctl(fd, DCMD_CC_GET_SAMPLE, &sample, sizeof sample, NULL);

// server (io_devctl):
out = (char *)&msg->o + sizeof(msg->o);       // reply payload sits after the header
memcpy(out, &W.s, sizeof(cc_sample_t));
msg->o.nbytes = sizeof(cc_sample_t);
return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(cc_sample_t));
```

That `devctl()` is a **synchronous `MsgSend()`**. The guardian blocks, the driver runs, replies, and the guardian continues. There is no shared memory to tear, and no lock to forget.

## 6. A 5 ms heartbeat: timers and pulses (15 min)

A *pulse* is a tiny, non-blocking message (8 bits of code plus 32 bits of value). The kernel can send one when a timer fires (`src/guardian.c`):

```c
int chid = ChannelCreate(0);
int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
struct sigevent ev;
SIGEV_PULSE_INIT(&ev, coid, 60, PULSE_TICK, 0);        // deliver at priority 60
timer_create(CLOCK_MONOTONIC, &ev, &tid);
its.it_value.tv_nsec = its.it_interval.tv_nsec = 5000000; // 5 ms, repeating
timer_settime(tid, 0, &its, NULL);

for (;;) {
    MsgReceivePulse(chid, &pulse, sizeof pulse, NULL);    // sleep until the tick
    uint64_t now = ClockCycles();                         // CPU cycle counter
    double interval_us = (now - last) * 1e6 / cycles_per_sec;
    jitter = fabs(interval_us - 5000);                    // how late (or early) were we?
    ...
}
```

The same loop also predicts. Every 20 ticks it fits a least-squares line to 4 s of readings and computes `seconds_to_breach = (8 °C − T) / slope`. When the grid is down and that drops under 60 s, it writes `backup_on` to the driver.

✅ **Checkpoint:** open `http://<ip>:8080`. The pill says **LIVE · QNX 8.0.0 x86_64**, and the proof strip shows the guardian's jitter in microseconds.

## 7. The experiment: flood the CPU (15 min)

`coldchaind` runs two copies of the 5 ms loop:

| thread | priority | policy | wakes by |
|---|---|---|---|
| guardian | 60 | FIFO | timer pulse |
| naive monitor | 10 | RR | `clock_nanosleep` |

```sh
pidin -p coldchaind          # see the priorities: 60f, 50r, 20f, 15r, 10r …
echo stress_on > /dev/coldchain
```

This starts one busy thread per CPU, plus one more, all at priority **11**: ordinary work that outranks the naive loop by a single level. It stops itself after 30 s, because your ssh shell also runs at priority 10 and freezes with it. In the dashboard:

- The **outer grey orb stops and turns red,** and its readout switches to `starved for 12.3 s`. Priority 11 beats priority 10 every time, so the naive loop gets no CPU at all until the flood ends.
- The **inner cyan orb keeps its pace.** The guardian outranks all of them, so the scheduler runs it the moment its pulse arrives.

> 💡 **Priority inheritance, the subtle part:** the driver thread is priority 50. When the guardian (60) sends it a `devctl`, the driver runs that request at **60**. If it didn't, a priority-55 task could delay the guardian's sensor read. This is *priority inversion*, the bug that famously reset the Mars Pathfinder lander. QNX channels inherit priority by default, and our `pthread_mutex` asks for `PTHREAD_PRIO_INHERIT` explicitly.

You can watch the inheritance happen. While `coldchaind` runs, look at thread 2 in `pidin -p coldchaind`: it was created at priority 50, but it shows **60r** because it's serving the guardian.

Write down the numbers from the proof strip, with and without the flood. These are your results.

> 📏 **An honest note on VMs.** In my VirtualBox VM on a Windows laptop, both loops showed millisecond-level jitter even when idle, with occasional spikes above 100 ms. That's the host pausing the whole virtual CPU, not QNX. QNX's tick was 1 ms (`pidin syspage=qtime`). The flood result is the one a VM can show faithfully. For microsecond numbers, use real hardware such as a Raspberry Pi 5.

## 8. Get proof from the kernel itself (15 min)

Dashboards can lie; kernel traces don't. QNX's instrumented kernel can log every context switch, message and interrupt. Our code adds its own events to that stream:

```c
trace_logf(_NTO_TRACE_USERFIRST + CC_TR_STATE, "%s -> %s T=%s", from, to, temp);
```

`trace_logf` doesn't support `%f`, so format numbers with `snprintf` first.

Capture a few buffers while the system runs:

```sh
echo stress_on > /dev/coldchain
sh /data/coldchain/trace.sh        # wraps: tracelogger -n 3 -f coldchain.kev, then traceprinter
```

Things that tripped me up, so they don't trip you:

- **Don't add `-d1`.** That's daemon mode: `tracelogger` waits for your program to start tracing itself, so it appears to hang forever.
- **`Resource busy`** means an interrupted capture is still holding the trace buffers. Run `slay -f tracelogger`.
- **`traceprinter` isn't in the default VM image.** Copy it from `qnx800/target/qnx/x86_64/usr/bin/traceprinter`. `deploy.bat` does this for you.
- **On Windows, keep `.sh` files with Unix line endings.** If a script fails with `: cannot execute - No such file or directory`, Git converted it to CRLF. The repo's `.gitattributes` prevents this.

The guardian's events appear right in the kernel log:

```
t:0x425f8293197 CPU:00 USREVENT :EVENT:102 STR:"guardian late interval_us=6243"
t:0x425f88fc09d CPU:01 USREVENT :EVENT:102 STR:"guardian late interval_us=3817"
```

Copy `coldchain.kev` to your PC and open it in the **System Profiler** (Momentics IDE or QNX Toolkit for VS Code). Find the `coldchaind` guardian thread. You'll see it preempting the flood threads every 5 ms, with your user events right beside it.

## 9. The dashboard: streaming a real-time system to Three.js (10 min)

The daemon's HTTP thread (priority 15, below the guardian) serves `/api/stream` as **Server-Sent Events**:

```c
"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n..."
for (;;) { send "data: {json}\n\n"; nanosleep(100 ms); }
```

```js
const es = new EventSource(base + '/api/stream');
es.onmessage = (e) => scene.update(JSON.parse(e.data));
```

The 3D orbits move by the **measured** tick counts (`guardian.ticks`, `naive.ticks`), so when the naive loop starves, the visualization stops with it. Three.js is vendored in `web/vendor`, so a target in a clinic with no internet still serves the whole UI.

## 10. Where to go next

- **Real hardware:** `make qnx-arm`. On a Raspberry Pi 5, replace `plant.c` with a DS18B20 1-Wire reader behind the same `/dev/coldchain`. The guardian doesn't change.
- **Adaptive partitioning:** give the guardian a guaranteed CPU budget even against higher-priority work.
- **Split processes:** move the driver into its own process and watch priority inheritance cross the process boundary in the System Profiler.

### Recap

You wrote a QNX driver, talked to it with `cat`, `echo` and `devctl`, built a pulse-driven control loop, proved it holds a 5 ms deadline under load with both a live chart and a kernel trace, and streamed it to a 3D web UI.

---
*Found a step that didn't work for you? Open an issue. This codelab was tested end to end on Windows 11 with QNX SDP 8.0 and a VirtualBox x86_64 VM, November 2026.*
