#!/bin/bash
# Regression test: a silent AI control channel must not wedge the dashboard gateway.
#
# A deadline cannot release a client that is reply-blocked on a channel created with
# _NTO_CHF_UNBLOCK (name_attach and resource managers do) unless the server answers. The AI is the one
# server we cannot make answer, so the gateway calls it through a helper thread. This stands up a server
# that receives a message and never replies, under the AI's name, and checks the gateway stays up.
#
#   tools/qemu/gateway-test.sh        (VM and stack running; needs the codelab example built:
#                                      docs/codelab/markdown/qnx-kernel-gate/src/build.sh)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
MUTE="$ROOT/docs/codelab/markdown/qnx-kernel-gate/src/out/mute"
[ -x "$MUTE" ] || { echo "build the codelab examples first (see the header of this script)"; exit 2; }
KEY="$HOME/.qnx/tether_vm_ed25519"
scp -q -i "$KEY" -P 2222 -o IdentitiesOnly=yes -o "UserKnownHostsFile=$ROOT/target/qemu/local/ssh-ident" "$MUTE" root@127.0.0.1:/tmp/mute

fails=0
check() { if [ "$1" = ok ]; then echo "PASS  $2"; else echo "FAIL  $2"; fails=$((fails+1)); fi; }
ms() { curl -s -m "$1" -o /dev/null -w '%{time_total}' "${@:2}" 2>/dev/null || echo 99; }

"$ROOT/tools/qemu/run-vm.sh" ssh 'chmod +x /tmp/mute; slay -s KILL tetherd; sleep 1; slay tether-ai; sleep 1; /tmp/mute server tether-ai >/tmp/mute.log 2>&1 &
 sleep 1; cat /tmp/mute.log' >/dev/null 2>&1

t1=$(ms 6 -X POST -H 'X-Tether: 1' 'localhost:8080/api/fault?name=nan')
check "$(awk -v t="$t1" 'BEGIN{print (t < 2.0) ? "ok" : "bad"}')" "call into the silent AI gave up after ${t1}s instead of hanging"
t2=$(ms 3 localhost:8080/api/status)
check "$(awk -v t="$t2" 'BEGIN{print (t < 0.5) ? "ok" : "bad"}')" "status page still answers (${t2}s)"
t3=$(ms 3 -X POST -H 'X-Tether: 1' 'localhost:8080/api/fault?name=nan')
check "$(awk -v t="$t3" 'BEGIN{print (t < 0.5) ? "ok" : "bad"}')" "a second call fails fast (${t3}s) instead of piling up"

"$ROOT/tools/qemu/run-vm.sh" ssh 'slay mute' >/dev/null 2>&1
"$ROOT/tools/qemu/stack.sh" start >/dev/null 2>&1
[ "$fails" -eq 0 ] && echo "ALL PASS" || echo "$fails FAILED"
exit "$fails"
