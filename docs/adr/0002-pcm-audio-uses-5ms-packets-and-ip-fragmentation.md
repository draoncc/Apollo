# PCM audio uses 5 ms packets and relies on IP fragmentation

Supersedes [ADR 0001](0001-pcm-surround-uses-shorter-packets.md).

PCM audio at 48 kHz/16-bit is 960 B per 5 ms packet in stereo, 2880 B in 5.1 and 3840 B in 7.1, so surround packets are larger than a single datagram on a typical 1500-byte MTU. We send them whole anyway, within a 4096-byte audio packet limit that matches Vibepollo's, and let IP fragmentation split them. The host fixes the packet duration at 5 ms whenever PCM audio is negotiated, whatever the client sent, so the audio codec alone tells the client how long a packet is. This keeps one audio packet per RTP packet, one packet duration for every channel layout, and 200 packets/s even in 7.1, and it lets Apollo and Vibepollo send PCM audio the same way, so one client implementation works against both hosts. The cost is that losing any one fragment loses the whole packet, and the 4+2 Reed-Solomon audio recovery can't rebuild a partially received packet, so on lossy networks surround PCM audio has more short gaps of silence than unfragmented packets would.

## Considered Options

- **Shortening the packet duration per channel layout so each packet fits in one datagram** (stereo 5 ms, 5.1 2 ms, 7.1 1 ms, as ADR 0001 chose): rejected. It means a packet duration that depends on the channel layout, up to 1000 packets/s for 7.1, and a second PCM audio wire format that differs from Vibepollo's.
- **Splitting each 5 ms frame across several RTP packets**: rejected. It needs a new header for the pieces, reassembly on the client, and recovery groups that no longer line up with whole frames.
