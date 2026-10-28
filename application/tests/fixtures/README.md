# Synthetic decoder regression fixture

`sine-with-cover.mp3` contains a two-second 440 Hz sine wave, mono 44.1 kHz, and an embedded 32 × 32 solid-color JPEG. It contains no user or licensed music. Created locally with the development-only `imageio-ffmpeg` FFmpeg generator (not distributed with the application).

This reproduces the Qt 6.8.3 QAudioDecoder failure where a file with an attached picture reports completion without any audio buffers. The same sine MP3 without the picture decodes normally. `decoder_regression.py` verifies actual duration and nonzero peaks, source preservation, Unicode paths, and invalid input rejection.

The same fixture also reproduces QMediaPlayer reporting playback while position remains zero on Qt 6.8.3. Playback regression tests require Qt 6.9.3 or newer; waveform analysis uses FFmpeg directly. Tail-damaged MP3 and truncated WAV cases are generated in isolated temporary directories by the regression script.
