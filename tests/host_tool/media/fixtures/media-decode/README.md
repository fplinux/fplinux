# Media decoder fixture

The Base64 fixture contains an owned two-second 440 Hz VBR MP3 test tone with the title `Finite tone` and artist `FPLinux tests`. Tests decode it into their temporary directories; no executable or game data is included.

The literal PCM vector in `media-decode.c` was independently decoded with FFmpeg. It allows small decoder rounding differences and checks the position after a one-second seek. The WAV expectations are literal signed sample values.

The AMR-NB fixture contains 180 ms of a synthetic tone in the 12.2 kbit/s mode. The corresponding little-endian PCM16 fixture was decoded independently with the FFmpeg 9.0.2 command-line program. The decoder test compares all samples and checks overlapping windows after seeks against those same reference bytes.
