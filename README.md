# NuclearSEQ
 A sequencer format and player that supports MIDI conversion to playback for the Nintendo DS's PSG sound chip.
 
 The Nintendo DS has an 8-channel Programmable Sound Generator (PSG) circuit within the ARM7TDMI sub-CPU that can be accessed on channels 8-15 (9-16 in MIDI), where channels 8-13 are pulse waves with variable duty cycle and channels 14 and 15 are noise. Both types of channel can have their frequency, volume, and panning adjusted in realtime, though only the pulse channels can alter their duty cycle.
 
 Because there are no easy ways to make music with these channels on real hardware using MIDI, this player and sequence format are setup to do so.

 ## Table of Contents
1. [Introduction](#nuclearseq)
2. [How to Use (Basic Overview)](#how-to-use-basic-overview)
3. [Sequencing Guidelines](#sequencing-guidelines)
4. [Duty Cycle](#duty-cycle)
5. [Creating Envelopes (Volume, Pitch, Slide)](#creating-envelopes-volume-pitch-slide)
6. [Changing Envelopes (Volume, Pitch, Slide)](#changing-envelopes-volume-pitch-slide)
7. [Looping](#looping)
8. [Building](#building)
9. [Credits](#credits)
10. [Additional Resources](#additional-resources)
 
 # How to Use (Basic Overview)
 1. Sequence a MIDI in your preferred MIDI editor (The `OCTAVE_SHIFT` macro assumes you use FL Studio but this can be altered).
 2. Run m2text.py (or .exe) and select your MIDI.
 3. Export the MIDI either to a folder of your choice with a unique name or to the `NuclearSEQ/seq/` folder and name it `song.txt` if you want to test it immediately.
    - If you do not have a `song.txt` in the `NuclearSEQ/seq/` folder, `demoSong.txt` will play instead.
    - Songs **MUST** be named `song.txt`, at least until I add support for custom song names using the keyboard.
    - You do not have to rebuild the .nds file to listen to your changed song or envelopes, both are reloaded when the .nds is started.
4. Open your preferred Nintendo DS emulator or Homebrew Launcher if using a real DS.
    - If on hardware, move the entire folder containing "NuclearSEQ.nds" and its subfolders to your flashcart's SD card.
5. Run the .nds and press any button to play the sequence.

# Sequencing Guidelines
- The quantization rate for note-length as well as automation is a 64th note. This means note lengths or automation changes that are smaller than a 64th note or note changes that occur on a non-64th note division will be skipped. To fix this, simply quantize all notes and automation changes to a 64th interval (1/4 step).
	- If you can help it, try avoiding notes that occur once every 64th as with an increase in BPM, a decrease in timing accuracy happens as well. 32nds are always safe, so if you need resolution, try multiplying your BPM by 2 and stretching your notes to compensate by 2. (e.g. a 64th at 120 BPM becomes a 32nd at 240 BPM).
- BPM changes mid-song are currently not supported, but might be if there is demand for it.
- Though this format uses MIDI, sequences are expected by the player to be made in a tracker-like format, meaning there is no dynamic channel allocation and only one note is expected to play at a time. Multiple notes played on one channel will *not* find an empty channel per note to allocate all notes; instead, only one will play and the others will be discarded.
    - Similarly, if two notes overlap each other, the first note will be cut off by the second note.
- Rapid automation changes in MIDIs, especially ones produced by FL Studio, may cause playback to slow slightly. If this occurs, optimize your automation changes by quantizing them so they do not occur every 64th.
- By default, the pitch bend range is +/- 12 semitones. This can be changed by altering the `PITCH_BEND_RANGE_SEMITONES` macro.
-Channels 9-16 in MIDI map to channels 8-15 on the DS. Other channels will not play sound.

- *For FL Studio users: If you cannot hear anything on Channel 10 when sequencing using Fruity LSD because it is always mapped as a drumkit, sequence on another channel and then change back to Channel 10 before exporting as MIDI.*

# Duty Cycle
- Changing Duty Cycle for the pulse wave channels is handled by the Program Change value per channel.
    - This will have no effect on noise channels 15 and 16.
- The following is a table of Program Change values along with their General MIDI instrument mappings and its corresponding Duty Cycle value according to `libnds/sound.h`

| Program Change (PC) | Instrument | Duty Cycle  |
| --------| -------- | ------ |
| 1 | Acoustic Grand Piano | 12% |
| 2 | Bright Acoustic Piano | 25% |
| 3 | Electric Grand Piano | 37% |
| 4 | Honky-tonk Piano | 50% |
| 5 | Electric Piano 1 | 62% |
| 6 | Electric Piano 2 | 75% |
| 7 | Harpsichord | 87% |
| 8 | Clavi | 0% (silence) |

# Creating Envelopes (Volume, Pitch, Slide)
Envelopes in NuclearSEQ are defined in the `envelopes.txt` file. Each envelope (with the exception of Slide envelopes) has a **unique numeric ID**, which is used in your MIDI sequence by sending CC messages to select which envelope a channel uses.

- Volume_Env#: **Attack** (in 64ths\*,), **Decay** (in 64ths\*), **Sustain** (from 0-127), **Release** (in 64ths\*)
    - If no Volume Envelope is defined, each note on the channel will play at a sustain level equal to the velocity of the note.

- Pitch_Env#: **Delay** (in 64ths\*), **Rate** (in 64ths\*), **Depth** (in Hz), **Ramp** (in Hz)

    - Use pitch envelopes to emulate the traditional use of the mod wheel.

- Slide_Env: **Kit Number** (1-16), **Trigger Note** (0-127 *or* -1 to activate `Relative Mode`), **Start Note** (0-127 in absolute mode, -128 to +127 in relative mode), **End Note** (0-127), **Duration** (in 64ths\*)
    - A trigger note of -1 will turn on `Relative Mode`, making *any* note activate a slide envelope *by default*, and the start and end notes will become relative
        - e.g. `Slide_Env: 1, -1, 12, 0, 12` means that when CC76 is set to 1, any note by default on the channel will have a slide envelope where playing any note will make the pitch of the channel first jump up to +12 semitones before falling back to 0 semitones over the course of twelve 64th intervals.

    - Relative mode can be overwritten by a slide envelope with the same kit number that has a **Trigger Note** value that is not -1.
    - It is advisable to put the `Relative Mode` envelope definition at the top of any set of Slide Envelopes for a particular kit number to prevent undefined behavior.
        
###### \* = Positive integers only.

# Changing Envelopes (Volume, Pitch, Slide)
- While volume, pitch bend, and panning automation are handled automatically, *envelopes* are not. You can change the envelopes that each channel will use by changing the Continuous Controller values of:
    - CC74: Volume
    - CC75: Pitch
    - CC76: Slide
- The value you should set CC74 and CC75 to is equivalent to the envelope that you want to activate in `NuclearSEQ/seq/envelopes.txt`
    - e.g. a CC74 value of 2 on Channel 10 will make the second pulse wave channel use `Volume_Env2` and its respective values from `envelopes.txt`
- CC76 on the other hand is meant for easy drumkit creation for the noise channels, though it can also be used for the pulse channels.
    - e.g. a CC76 value of 2 on Channel 15 will activate all slide envelopes that have 2 as their kit number.
    - Change CC76 to the *set* of slide envelopes you'd like to use. See the "Creating Envelopes" section for an explanation.

# Looping
- On any channel, if the notes C0 or C#0 are played, the loop points will be defined. These notes are silent during playback
    - C0 is Loop Start, C#0 is Loop End.
        - Have only *one* pair of these per sequence.
- If no loop point is defined, the track will end at the last note.

# Building
1. Install BlocksDS via https://blocksds.skylyrac.net/docs/setup/options/
    - Step 4 in this guide is **NOT OPTIONAL** as this project uses NightFox's Lib.
2. Navigate to the top-level NuclearSEQ folder in the Wonderful Toolchains shell.
3. In the Wonderful Toolchains shell, run ``make clean && make``
4. Patch the generated ROM to use DLDI with any DLDI ROM patcher.
    - Recommended: https://www.chishm.com/DLDI/dldirc.html
    - Most homebrew launchers on the real console will also do this for you if the ROM is detected to not be patched.

# Credits
- Nuclear (me) - https://www.youtube.com/@NUKELEDGE
- BlocksDS developers for their amazing and easy-to-use build system - https://blocksds.skylyrac.net/docs/
- AntonioND for making NightFox's Lib: https://github.com/knightfox75/nds_nflib

# Additional Resources
- https://blocksds.skylyrac.net/docs/libnds/sound_8h.html

# Known Bugs
- Sometimes really fast note-offs are missed at higher tempos.
- Pan law is not yet implemented (Debating implementing this in the convertor or the player itself).
