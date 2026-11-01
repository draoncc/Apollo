# Apollo

Apollo is a game-streaming host that sends video, audio and input to Moonlight-protocol clients, Artemis among them.

## Audio

**Audio codec**:
The audio encoding a host and client agree on for a session: either Opus audio or PCM audio.
_Avoid_: Audio format, audio mode

**Opus audio**:
Audio sent as Opus multistream packets. Every client supports it, and it is the fallback codec when PCM audio isn't both offered and chosen.
_Avoid_: Compressed audio

**PCM audio**:
Audio sent as raw 16-bit signed samples at 48 kHz with no compression of any kind. Apollo offers it in every session; it is used only when the client chooses it.
_Avoid_: Uncompressed audio, lossless audio, raw audio

**Packet duration**:
How many milliseconds of audio one audio packet carries. The client chooses it; for PCM audio it follows from the channel layout.
_Avoid_: Frame size, packet size

**Channel layout**:
How many speaker channels the audio carries: stereo, 5.1 or 7.1.
_Avoid_: Surround mode, channel config
