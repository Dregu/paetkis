# pätkis

A Hyprland bar so small it's not even a bar, just a provider for some pango content to be piped into [this fork of ergo](https://github.com/Dregu/ergo/), which is actually a bar with no content so that's like a brilliant coincidence.

Vaguely shows what's on your workspaces and a Finnish clock. It's not rust, but it's still 🚀blazingly🏃💨fast⚡️ so you can just bind it straight to holding a key:

```lua
-- show overlay bar called bar while holding left SUPER
hl.layer_rule{match = { namespace = 'bar' }, animation = 'slidefade'}
hl.bind('SUPER + Super_L', hl.dsp.exec_cmd('paetkis | exec -a bar ergo -l 3 -f "Hack Nerd Font Mono 12" -n bar'), {locked = true, non_consuming = true, transparent = true, ignore_mods = true, dont_inhibit = true})
hl.bind('SUPER + Super_L', hl.dsp.exec_cmd('pkill -f "^bar "'), {release = true, locked = true, non_consuming = true, transparent = true, ignore_mods = true, dont_inhibit = true})
```
