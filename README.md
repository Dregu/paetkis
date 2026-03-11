# pätkis

A Hyprland bar so small it's not even a bar, just a provider for some pango content to be piped into [this fork of ergo](https://github.com/Dregu/ergo/), which is actually a bar with no content so that's like a brilliant coincidence.

Vaguely shows what's on your workspaces and a Finnish clock. It's not rust, but it's still 🚀blazingly🏃💨fast⚡️ so you can just bind it straight to holding a key:

```ini
# show overlay bar called bar while holding SUPER
layerrule = animation slidefadevert, match:namespace bar
bindltinp = SUPER, Super_L, exec, paetkis | exec -a bar ergo -f "Hack Nerd Font Mono 12" -n bar
bindltinpr = SUPER, Super_L, exec, pkill -f "^bar "
```
