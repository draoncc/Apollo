# PCM surround audio uses shorter packets instead of splitting packets

Status: superseded by [ADR 0002](0002-pcm-audio-uses-5ms-packets-and-ip-fragmentation.md).

PCM audio at 48 kHz/16-bit doesn't fit in the 1400-byte audio packet limit at the usual 5 ms packet duration once there are more than two channels (5.1 needs 2880 B, 7.1 needs 3840 B). We chose to shorten the packet duration per channel layout instead. The client sets it when it chooses PCM audio, because it already owns the packet duration and nothing reaches it from the host after ANNOUNCE; the host only checks that the packets fit: stereo uses 5 ms (960 B), 5.1 uses 2 ms (1152 B) and 7.1 uses 1 ms (768 B). This keeps one audio packet per RTP packet, so the wire format, the millisecond RTP timestamps and the 4+2 Reed-Solomon audio recovery groups all work unchanged, and the client needs no reassembly. The cost is a higher packet rate, up to 1000 packets/s for 7.1.

## Considered Options

- **Splitting each 5 ms frame across several RTP packets**: rejected. It needs a new header for the pieces, reassembly on the client, and recovery groups that no longer line up with whole frames.
- **Raising the packet size limit and letting IP fragmentation split packets**: rejected. Losing any fragment loses the whole packet, and the audio recovery data can't help with that.
