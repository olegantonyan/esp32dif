# S/PDIF over Wi-Fi vs Bluetooth aptX HD: handoff

Status as of 2026-11-08: codec measured, level match not yet run on real hardware.

## Question

On the Edifier S2000MKII, S/PDIF over Wi-Fi sounds better than Bluetooth aptX HD: slightly deeper bass, crisper highs. A proper ABX isn't possible because Bluetooth takes too long to reconnect. Is the difference real, and if so, where does it come from?

The user ABXes FLAC vs MP3 320 at about 70% on Audio-Technica ADX500 headphones, but not on these speakers.

## Conclusion so far

The codec alone can't explain deeper bass. Its high-frequency error is small and borderline audible at most. "Deeper bass + crisper highs" is what slightly louder playback sounds like. Level mismatch is the top suspect, followed by the speaker processing each input differently and listening position.

## aptX HD round-trip measurements

Tool: `tools/aptx-roundtrip [music file]`. It uses ffmpeg's aptX HD (the same algorithm as PipeWire's libfreeaptx) at 48 kHz, 576 kb/s, with a fixed 90-sample delay.

| | aptX HD result | Smallest detectable change (perfect ear) | Audible? |
|---|---|---|---|
| Frequency response, 10 Hz–15 kHz | flat within ±0.05 dB | ~0.3–0.5 dB | no |
| Frequency response, 15–22 kHz | +0.05 to +0.13 dB | ~0.5 dB; adult hearing up there is poor | no |
| Gain | −0.002 dB | ~0.2–0.3 dB | no |
| Distortion + noise, 30/50/100 Hz sines at −3 dBFS | −97 dB | far below audibility | no; nothing in the bass |
| Distortion + noise, 1 / 10 / 15 / 19 kHz sines | −71 / −55 / −48 / −38 dB | hidden by the music on real recordings | no |
| Output on digital silence | −126 dBFS | — | no |
| Pink noise, signal-to-error ratio per band | 55–64 dB below 320 Hz, 30 dB at 5–10 kHz, 24 dB at 10–16 kHz, 18 dB at 16–20 kHz | — | — |
| Music ("Take Five" MP3), signal-to-error ratio | 58 dB overall; 24 dB at 10–16 kHz, 22 dB at 16–20 kHz | the error has to sit roughly 20 dB below the music in its band to stay hidden | borderline: only for sustained pure high tones, mostly on headphones |

aptX HD doesn't use a hearing model: it adds fixed ADPCM noise that grows with frequency. That makes highs slightly rougher, not duller. MP3 320 shapes its noise to hide under the music, so its per-band numbers aren't directly comparable.

## Factors, ranked

1. **Level difference.** A 0.2–0.3 dB difference already shifts preference without sounding louder. Bass and treble get louder faster than the midrange as volume rises. At the time of writing, the S/PDIF sink was at 0.92 on wpctl's cubic scale, which is −2.2 dB. The Bluetooth path has several unknowns:
   - the PipeWire sink volume
   - the speaker volume, which Bluetooth can set remotely
   - phone EQ and effects, if a phone is the source
   - the speaker's input gain
2. **The speaker processing each input differently.** The S2000MKII's DSP may treat Bluetooth and optical/coax differently. That would be a real, measurable difference.
3. **Listening position.** Room resonances change the bass by several dB over 20–50 cm of head movement. During a slow switch you rarely sit in the same place.
4. **Memory and expectation.** Fine detail about timbre fades from memory within seconds, so a reconnect gap makes the comparison unreliable. The listener also knows which input is playing, and built one of them.
5. **Ruled out:**
   - Resampling: 44.1 → 48 kHz in both paths, measured SINAD 138 dB (README).
   - S/PDIF jitter: the WM8804's is tens of picoseconds; detection needs hundreds of nanoseconds.
   - PLL trim: 5 ppm steps; the smallest audible pitch change is about 2000 ppm.
   - The occasional 8.6 ms Wi-Fi dropout: it can click, but it doesn't change the tone.

ABX statistics: with 20 trials, 14 correct gives p≈0.06 and 15 correct gives p≈0.02. Report the trial count, not just a percentage.

## Next steps

### 1. Level and per-input frequency response: `tools/level-match`

The laptop mic only needs to measure differences, not absolute level. The mic's own response and the room cancel because both inputs go through the same chain. The tool plays pink noise (−20 dBFS RMS) to the default sink and records it from the default source. It saves octave-band levels to `~/.cache/level-match/<label>.json` and prints the per-band difference against earlier runs. It also prints the `wpctl set-volume` value that matches the levels.

1. Pause music. Put the laptop at the listening position and don't move it or touch the speaker knobs.
2. Make S/PDIF the default sink and run `tools/level-match spdif`.
3. Connect Bluetooth and check the codec: `pactl list sinks | grep -i codec`. Make it the default sink and run `tools/level-match bt`.
4. Switch back to S/PDIF and run `tools/level-match spdif2`. The bands should agree with `spdif` within about ±0.1 dB; that's the noise floor of the measurement.
5. Set the S/PDIF sink to the suggested volume, which is fine-grained software volume. Per-band differences larger than the step 4 noise floor mean the speaker treats the inputs differently.

Checked only on synthetic recordings: it recovered an injected −0.40 dB level change and a bass shelf exactly. It assumes the PC is the Bluetooth source; a phone source needs a record-only mode, which isn't written yet.

### 2. Codec-only ABX with instant switching

Do this on the ADX500, the most sensitive setup: if the difference can't be heard there, the codec is cleared. Use a FLAC source, not MP3.

```
ffmpeg -i track.flac -ar 48000 -c:a flac ref.flac
ffmpeg -i ref.flac -c:a aptx_hd -f aptx_hd - | ffmpeg -f aptx_hd -ar 48000 -i - -af atrim=start_sample=90 -c:a flac aptxhd.flac
```

The two files are sample-aligned; the residual between them is −60 dB on music. Linux has no foobar ABX; a small command-line ABX player is an open idea.

### 3. Then, if a difference remains

After levels are matched within 0.1 dB, run a blind comparison with a helper switching inputs. Keep your head fixed, use short excerpts, and record trial counts. Accept that the reconnect gap lowers sensitivity.
