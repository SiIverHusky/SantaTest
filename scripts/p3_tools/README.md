# P3 Audio Format Conversion and Playback Tools

This directory contains Python scripts for handling P3 format audio files:

## 1. Audio Conversion Tool (convert_audio_to_p3.py)

Converts regular audio files to P3 format (4-byte header + Opus packet stream structure) with loudness normalization.

### Usage

```bash
python convert_audio_to_p3.py <input_audio_file> <output_p3_file> [-l LUFS] [-d]
```

The optional `-l` flag specifies the target loudness for normalization (default: -16 LUFS); `-d` flag disables loudness normalization.

Consider using `-d` to disable loudness normalization if the input audio meets any of these conditions:
- Very short audio
- Already loudness-normalized audio
- Audio from default TTS (current TTS system defaults to -16 LUFS)

Example:
```bash
python convert_audio_to_p3.py input.mp3 output.p3
```

## 2. P3 Audio Player (play_p3.py)

Plays P3 format audio files.

### Features

- Decodes and plays P3 format audio files
- Applies fade-out effect when playback ends or is interrupted
- Supports command-line specification of files to play

### Usage

```bash
python play_p3.py <p3_file_path>
```

Example:
```bash
python play_p3.py output.p3
```

## 3. Audio Conversion Back Tool (convert_p3_to_audio.py)

Converts P3 format back to regular audio files.

### Usage

```bash
python convert_p3_to_audio.py <input_p3_file> <output_audio_file>
```

Output audio file must include an extension.

Example:
```bash
python convert_p3_to_audio.py input.p3 output.wav
```

## 4. Batch Audio/P3 Conversion Tool

A GUI tool supporting batch conversion between audio and P3 formats.

![](./img/img.png)

### Usage:
```bash
python batch_convert_gui.py
```

## Dependencies Installation

Before using these scripts, ensure required Python libraries are installed:

```bash
pip install librosa opuslib numpy tqdm sounddevice pyloudnorm soundfile
```

Or use the provided requirements.txt file:

```bash
pip install -r requirements.txt
```

## P3 Format Specification

P3 is a simple streaming audio format with the following structure:
- Each audio frame consists of a 4-byte header and an Opus-encoded data packet
- Header format: [1 byte type, 1 byte reserved, 2 bytes length]
- Fixed sample rate: 16000Hz, mono channel
- Frame duration: 60ms