id: qnx-kernel-gate
title: Keep an Untrusted Process Away from a Device on QNX 8.0
summary: Build a resource manager, let the kernel refuse a process it should refuse, lock that process down, learn the moment it dies, and bound a call to a server that stops answering
categories: qnx, ipc, security
tags: intermediate
difficulty: 2
status: published
authors: ZNLong2203
feedback_link: https://github.com/qnx/codelabs/issues


# Keep an Untrusted Process Away from a Device on QNX 8.0

## Welcome
Duration: 2:00

### Overview
Sooner or later a system has a process you do not fully trust: a machine-learning model, a plug-in, a third-party library running in its own process. This codelab shows how to put such a process behind a gate that **the QNX kernel enforces**, so safety does not depend on the process behaving well.

You will build five small programs:

| Program | What it shows |
|---|---|
| `valve-rm` | A resource manager that owns a device node only root may open |
| `intruder` | The kernel refusing an ordinary user, with no code of yours involved |
| `sandboxed` | A process that starts as root, locks away abilities, then drops to an ordinary user |
| `watcher` and `talker` | A server that learns the moment a client dies, from a kernel pulse |
| `mute` | Why a call timeout does not always fire, and two ways to make it hold |

### What you will learn
- Create a device with `resmgr` and `iofunc`, and answer `devctl()` commands.
- Let the file mode and owner of a node do the access control.
- Use `procmgr_ability()` to deny and lock abilities, then `setuid()`.
- Receive `_PULSE_CODE_DISCONNECT` from a channel made by `name_attach()`.
- Use `TimerTimeout()`, and know when it cannot release a client.

These are the building blocks of **Tether**, a hackathon project that keeps an AI controller away from a simulated ventilator's actuator. Everything here was run on QNX SDP 8.0.

---

## Set Up Environment
Duration: 6:00

### Pre-requisites
1. A free QNX non-commercial licence and QNX SDP 8.0 installed. If you do not have them yet, follow the [Get a Free Non-Commercial QNX License](https://qnx.github.io/codelabs/get-non-commercial-qnx-license/index.html) codelab.
2. A QNX 8.0 target you can log in to. A board works. So does the QEMU image that `mkqnximage` builds (see the QNX documentation for `mkqnximage`).
3. A way to copy files to the target, such as `scp`, and a shell on it, such as `ssh`.

### Get the programs
The folder `src/` next to this codelab holds every file shown below. You can also create each file as you reach it. One header and six C files are used:

```
valve.h  valve-rm.c  intruder.c  sandboxed.c  watcher.c  talker.c  mute.c
```

### Build script
Create `build.sh` next to them:

```bash
@@src/build.sh@@
```

Load the SDP environment and build:

```bash
source ~/qnx800/qnxsdp-env.sh
chmod +x build.sh
./build.sh
```

The programs land in `out/`. On an x86_64 target use `ARCH=gcc_ntox86_64 ./build.sh`.

### Copy them to the target
```bash
ssh root@<target> mkdir -p /tmp/codelab
scp out/* root@<target>:/tmp/codelab/
ssh root@<target>
```

On the target, `cd /tmp/codelab` and `chmod +x *`. If your QEMU image forwards ssh to another port, add `-P <port>` to `scp` and `-p <port>` to `ssh`.

We use user ID **2002** as the ordinary, untrusted user. It does not need an entry in the password file: the `on` utility can run a program as any ID.

---

## A Device That Belongs to Root
Duration: 10:00

A resource manager is a process that adds a name to the path space, such as `/dev/guard/valve`. Programs reach it with `open()` and `devctl()`. Our valve has one number, its opening in percent.

### The shared header
```c
@@src/valve.h@@
```

`__DIOT` marks a command that sends data to the device and `__DIOF` one that reads it back.

### The resource manager
```c
@@src/valve-rm.c@@
```

Three things matter here:

1. **The node's owner and mode.** `iofunc_attr_init(&attr, S_IFCHR | 0600, ...)` with `uid` and `gid` set to 0 makes the node readable and writable by root only. Nothing else in the program decides who may open it.
2. **`do_devctl`.** It first calls `iofunc_devctl_default()` for the standard commands, then handles our two. `_RESMGR_PTR` sends the reply.
3. **`do_open`.** It calls `iofunc_open_default()`, which compares the caller's user and group with the node's mode. We only look at the result to count refusals.

### Run it
```bash
./valve-rm &
ls -l /dev/guard
```

You should see the server announce itself, and a node that only root can use:

```
serving /dev/guard/valve
crw-------  1 root root  0,   1 ... valve
```

---

## Let the Kernel Say No
Duration: 6:00

### The intruder
```c
@@src/intruder.c@@
```

### Run it twice
As root, then as the ordinary user:

```bash
./intruder
on -u 2002:2002 ./intruder
```

```
running as uid 0
opened the valve and set it to 100%: done
running as uid 2002
open /dev/guard/valve: Permission denied
```

Look at the resource manager's output:

```
valve set to 100%
the kernel's check refused an open (1 so far)
```

The refusal happened inside `iofunc_open_default()`, using only the node's mode and owner. The intruder never reached `do_devctl`, and nothing we wrote had to anticipate it.

---

## Take Away What a Process Does Not Need
Duration: 10:00

Refusing the device is one gate. A process you distrust should also lose the ability to make new ones, for example by starting other programs. QNX calls these **abilities**, and `procmgr_ability()` can deny them and lock the decision.

### The sandboxed process
```c
@@src/sandboxed.c@@
```

The order is the point:

1. Open what you need while you are still root.
2. Deny and lock abilities. `PROCMGR_ADN_NONROOT` says the rule applies once the process is no longer root, `PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK` denies and freezes it, and the list ends with `PROCMGR_AID_EOL`.
3. Drop to an ordinary user with `setgid()` and `setuid()`.

### Run it with and without the lock
```bash
./sandboxed
./sandboxed --no-lock
```

```
now uid 2002, abilities locked
the fd opened before:        still works
a fresh open now:            open: Permission denied
fork:                        Operation not permitted

now uid 2002, abilities left alone
the fd opened before:        still works
a fresh open now:            open: Permission denied
fork:                        allowed
```

Two lessons:

- Only the lock stops `fork()`. An ordinary user may fork by default, so denying it is a real restriction.
- **The file descriptor opened before the drop still works.** Access is checked at `open()`, not at each `devctl()`. Close what a process no longer needs before it drops privileges.

Each ability has its own `PROCMGR_AID_*` name. The QNX C Library Reference for `procmgr_ability()` lists them, and the SDP installs that page under `target/qnx/usr/help`.

---

## Know the Moment a Client Dies
Duration: 8:00

A server often needs to clean up when a client disappears. You do not have to poll: the kernel tells you.

### A server that watches
```c
@@src/watcher.c@@
```

`name_attach()` creates a channel and registers a name. As the QNX documentation for `name_attach()` explains, the channel is created with `_NTO_CHF_DISCONNECT` among its flags, so when the last connection from a client goes away the kernel delivers a `_PULSE_CODE_DISCONNECT` pulse. A pulse arrives from `MsgReceive()` with a receive ID of 0, which is how the loop tells it from a message.

### A client that crashes
```c
@@src/talker.c@@
```

### Run them
```bash
./watcher &
./talker
```

```
sent "hello 1", the reply status was 2
sent "hello 2", the reply status was 2
sent "hello 3", the reply status was 2
crashing now
Memory fault (core dumped)
```

and in the watcher's output:

```
watcher ready
message from pid ...: "hello 1"
message from pid ...: "hello 2"
message from pid ...: "hello 3"
a client went away: the kernel sent a disconnect pulse (scoid ...)
```

Notice that the reply status is **2**, the number of bytes `watcher` passed to `MsgReply()`. `MsgSend()` returns the status given to `MsgReply()`, so passing the reply length is a convenient convention.

---

## A Deadline That Does Not Always Fire
Duration: 12:00

A client that calls a server which never answers is stuck. `TimerTimeout()` puts a deadline on the next blocking call. Let us see when it works.

### The mute server and its client
```c
@@src/mute.c@@
```

### Case 1: a server that stops answering
```bash
./mute server &
./mute client bounded &
sleep 3
pidin -F "%a %N %J" | grep mute
```

```
 ...  mute      SIGWAITINFO
 ...  mute      REPLY
```

The server printed `got a message and I will not reply`. The client set a 200 ms deadline and is still in the `REPLY` state seconds later. **The deadline did not fire.**

The QNX documentation for `TimerTimeout()` explains why. If the server already received the message and created its channel with `_NTO_CHF_UNBLOCK`, `MsgSend()` does not unblock on its own. The kernel sends the server an unblock pulse instead, and it is up to the server to reply or call `MsgError()`. `name_attach()` and every resource manager create their channels with that flag.

Stop the pair with `slay -f mute`.

### Case 2: a server that answers the unblock pulse
This version keeps listening and releases the client when the pulse arrives:

```bash
./mute polite &
./mute client bounded
```

```
MsgSend failed after 203 ms: Connection timed out
```

The server printed `unblock pulse: releasing the client`. Now the deadline holds. This is the right fix **when you write the server**.

### Case 3: a server you do not control
If the server is the untrusted process, you cannot make it answer. Make the client survive it instead: give every call to that server to one helper thread, and let the caller give up while the helper stays stuck. This is a sketch of the idea:

```c
/* The caller waits at most 400 ms; the helper may block for as long as the server likes. */
long call_untrusted(const void *msg, size_t n, void *reply, size_t rn)
{
    pthread_mutex_lock(&job.mu);
    if (job.busy) {                /* an earlier call is still stuck */
        pthread_mutex_unlock(&job.mu);
        errno = EBUSY;
        return -1;
    }
    hand_job_to_helper(msg, n, rn);
    wait_on_condvar_with_deadline(&job.done, 400 /* ms */);
    if (!job.finished) {           /* the helper is still inside MsgSend() */
        job.abandoned = 1;         /* it will discard its answer when it returns */
        pthread_mutex_unlock(&job.mu);
        errno = ETIMEDOUT;
        return -1;
    }
    copy_reply(reply, rn);
    job.busy = 0;
    pthread_mutex_unlock(&job.mu);
    return job.result;
}
```

Tether's dashboard gateway uses this pattern to talk to its AI process. With the AI's control channel silent, the first call returned an error after 0.4 s, the status page still answered in 8 ms, and later calls failed in about 2 ms instead of piling up.

---

## Wrap Up
Duration: 3:00

### What each tool gave you
| Goal | Tool | Where you saw it |
|---|---|---|
| Only root may use the device | node mode and owner, checked by `iofunc_open_default()` | `intruder` |
| A process loses what it does not need | `procmgr_ability()` deny and lock, then `setuid()` | `sandboxed` |
| Learn that a client died | `_PULSE_CODE_DISCONNECT` on a `name_attach()` channel | `watcher` and `talker` |
| Never wait forever on a server | `TimerTimeout()`, plus the unblock pulse or a helper thread | `mute` |

### Clean up
```bash
slay -f valve-rm
slay -f watcher
slay -f mute
```

### Where to go next
- Combine the pieces: **Tether** runs an AI controller as an ordinary user behind exactly this kind of gate, with a verified arbiter in front of the actuator.
- Read the QNX documentation for `resmgr_attach()`, `iofunc_open_default()`, `name_attach()`, `TimerTimeout()` and `procmgr_ability()` for the options this codelab left out.
