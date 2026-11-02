# Apollo

Apollo is a game-streaming host that sends video, audio and input to Moonlight-protocol clients, Artemis among them.

## Audio

**Audio codec**:
The audio encoding a host and client agree on for a session: either Opus audio or PCM audio.
_Avoid_: Audio format, audio mode

**Opus audio**:
Audio sent as Opus multistream packets. Every client supports it, and it is the fallback codec when the client doesn't accept PCM audio.
_Avoid_: Compressed audio

**PCM audio**:
Audio sent as 16-bit signed samples at 48 kHz with no compression of any kind, always in 5 ms packets. Apollo offers it in every session; it is used only when the client accepts it.
_Avoid_: Uncompressed audio, lossless audio, raw audio

**Packet duration**:
How many milliseconds of audio one audio packet carries. The client chooses it for Opus audio; PCM audio always uses 5 ms.
_Avoid_: Frame size, packet size

**Channel layout**:
How many speaker channels the audio carries: stereo, 5.1 or 7.1.
_Avoid_: Surround mode, channel config
