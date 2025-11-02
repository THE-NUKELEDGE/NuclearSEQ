# MIDI to NuclearSEQ TXT Converter

A simple Python tool to convert a MIDI file into a text format compatible with NuclearSEQ (Nintendo DS PSG sequencer).

## Requirements

- Python 3.x
- `mido` library (`pip install mido`)
- `tkinter`

## Usage

1. Run the script:
```bash
python midi_to_txt.py
```
2. A file dialog will appear asking you to select a MIDI file (.mid or .midi) as input.
3. Another file dialog will appear asking where to save the TXT file. The output file will contain all the note events, controllers, and envelopes in NuclearSEQ format.

## Output
A text file (.txt) containing:
- BPM
- Note events (channel, program, note, velocity, start/end in 64th notes)
- Controller values (pan, pitch, volume, envelopes CC74/CC75/CC76)
- Duplicate lines are automatically removed.