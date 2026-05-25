#!/system/bin/sh
MODDIR=${0%/*}
RUNDIR=/dev/.15f1c4b9
OLD_RUNDIR=/dev/scene-netns-isolator

mkdir -p "$RUNDIR"
chmod 0755 "$RUNDIR"
rm -f "$RUNDIR"/.endpoint "$RUNDIR"/.pid "$RUNDIR"/.proxy_port "$RUNDIR"/[0-9a-f]* 2>/dev/null
umount "$OLD_RUNDIR/scene.netns" 2>/dev/null
rm -rf "$OLD_RUNDIR" 2>/dev/null

chmod 0755 "$MODDIR/bin" 2>/dev/null
chmod 0755 "$MODDIR/bin/scene-netnsctl" 2>/dev/null
chmod 0755 "$MODDIR/bin/su" 2>/dev/null
