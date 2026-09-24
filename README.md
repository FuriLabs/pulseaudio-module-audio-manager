# pulseaudio-module-audio-manager

## CLI

```sh
pactl load-module module-audio-manager-card
pactl list modules short
pactl list cards short
pactl list sinks short
pactl list sources short
pactl list message-handlers
pactl list cards
pactl list sinks
pactl list sources

pactl set-default-sink audio-manager-output
pactl set-default-source audio-manager-input

pactl set-sink-port audio-manager-output output-earpiece
pactl set-sink-port audio-manager-output output-speaker
pactl set-sink-port audio-manager-output output-headphones
pactl set-sink-port audio-manager-output output-headset

pactl set-source-port audio-manager-input input-internal-mic
pactl set-source-port audio-manager-input input-headset-mic

pactl send-message /audio-manager-card get-playback-role
pactl send-message /audio-manager-card playback-role-low-latency
pactl send-message /audio-manager-card playback-role-power-saving

pactl set-card-profile audio-manager-card voicecall
pactl set-card-profile audio-manager-card voicecall-bluetooth
pactl set-card-profile audio-manager-card voicecall-usb
pactl set-card-profile audio-manager-card default

pactl get-sink-volume audio-manager-output
pactl set-sink-volume audio-manager-output 50%
pactl get-sink-mute audio-manager-output
pactl set-sink-mute audio-manager-output 1
pactl set-sink-mute audio-manager-output 0

pactl get-source-mute audio-manager-input
pactl set-source-mute audio-manager-input 1
pactl set-source-mute audio-manager-input 0

pactl list sources
pactl list source-outputs

pactl list modules short
pactl unload-module <module-id>
```
