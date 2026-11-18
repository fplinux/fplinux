# Media decoder fixture

The Base64 fixture contains an owned two-second 440 Hz VBR MP3 test tone with the title `Finite tone` and artist `FPLinux tests`. Tests decode it into their temporary directories; no executable or game data is included.

The literal PCM vector in `media-decode.c` was independently decoded with FFmpeg. It allows small decoder rounding differences and checks the position after a one-second seek. The WAV expectations are literal signed sample values.
