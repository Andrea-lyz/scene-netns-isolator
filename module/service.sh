#!/system/bin/sh
MODDIR=${0%/*}
CTL="$MODDIR/bin/scene-netnsctl"
RUNDIR=/dev/.15f1c4b9
OLD_RUNDIR=/dev/scene-netns-isolator

mkdir -p "$RUNDIR"
chmod 0755 "$RUNDIR"
umount "$OLD_RUNDIR/scene.netns" 2>/dev/null
rm -rf "$OLD_RUNDIR" 2>/dev/null

if [ ! -x "$CTL" ]; then
  log -t scene-netns "scene-netnsctl is missing or not executable"
  exit 0
fi

if "$CTL" status >/dev/null 2>&1; then
  exit 0
fi

nohup "$CTL" pin >/dev/null 2>&1 &
sleep 1
"$CTL" status >/dev/null 2>&1 || log -t scene-netns "failed to pin Scene netns"
