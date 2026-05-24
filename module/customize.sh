SKIPUNZIP=0

ui_print "Scene Netns Isolator"
ui_print "Target package: com.omarea.vtools"
ui_print "Requires Zygisk and root network namespace support."

set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/bin/scene-netnsctl" 0 0 0755
set_perm "$MODPATH/bin/su" 0 0 0755
