#!/bin/bash
# Runs inside the build container; started by install-sdp.sh. Not meant to be
# run on its own.
#
# Command-line syntax follows the QNX Software Center user guide:
#   qnxsoftwarecenter_clt @options -mirror
#   qnxsoftwarecenter_clt @options -destination DIR -installBaseline com.qnx.qnx800
# The guide gives no SDP 8.0 example, so the package id is inferred from the
# 7.1 pattern (com.qnx.qnx710). Each step reports its own exit code.
set -uo pipefail

QSC_ROOT=/home/$(id -un)/qnx
CLT=$QSC_ROOT/qnxsoftwarecenter/qnxsoftwarecenter_clt
LOG=$QSC_ROOT/install.log
SDP=/opt/qnx800
OPTS=/secrets/qsc.opts

say() { printf '\n== %s\n' "$*" | tee -a "$LOG"; }

# 1. QNX Software Center. The installer shows the licence and asks y/n itself.
if [ ! -x "$CLT" ]; then
  say "Installing QNX Software Center (you will be asked to accept the licence)"
  bash "/inst/$INSTALLER" disable-auto-start "$QSC_ROOT" || { echo "QSC install did not finish"; exit 1; }
fi
[ -x "$CLT" ] || { echo "qnxsoftwarecenter_clt not found at $CLT"; exit 1; }

# 2. Credentials: typed here, written only to a tmpfs file with mode 0600.
umask 077
read -r -p "myQNX e-mail: " MQ_USER
read -r -s -p "myQNX password (not shown): " MQ_PASS
echo
printf '%s\n' "-myqnx.user=$MQ_USER" "-myqnx.password=$MQ_PASS" > "$OPTS"
unset MQ_PASS MQ_USER
trap 'rm -f "$OPTS"' EXIT

clt() { "$CLT" "@$OPTS" "$@" 2>&1 | tee -a "$LOG"; return "${PIPESTATUS[0]}"; }

say "1/4 mirror metadata"
clt -mirror; rc=$?; echo "exit=$rc" | tee -a "$LOG"
[ $rc -eq 0 ] || { echo "mirror failed: wrong credentials, no licence deployed, or no network"; exit $rc; }

say "2/4 activate the licence deployed to this account"
clt -syncLicenseKeys -activateAll -listLicenseKeys; echo "exit=$?" | tee -a "$LOG"

say "3/4 install SDP 8.0 into $SDP (several GB, can take a long time under emulation)"
clt -destination "$SDP" -installBaseline com.qnx.qnx800; rc=$?; echo "exit=$rc" | tee -a "$LOG"
[ $rc -eq 0 ] || { echo "install failed; see $LOG"; exit $rc; }

say "4/4 check"
if [ -f "$SDP/qnxsdp-env.sh" ]; then
  echo "OK: $SDP/qnxsdp-env.sh exists" | tee -a "$LOG"
else
  echo "MISSING: $SDP/qnxsdp-env.sh. Listing what was installed:" | tee -a "$LOG"
  ls "$SDP" | head -20 | tee -a "$LOG"
  exit 1
fi
