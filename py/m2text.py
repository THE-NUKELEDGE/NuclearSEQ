from mido import MidiFile, tempo2bpm
import math
import tkinter as tk
from tkinter import filedialog

def get_bpm(mid):
    bpm = 120.0
    for track in mid.tracks:
        for msg in track:
            if msg.type == 'set_tempo':
                bpm = tempo2bpm(msg.tempo)
                return bpm
    return bpm

def midi_to_txt(midi_path, output_path):
    mid = MidiFile(midi_path)
    bpm = get_bpm(mid)
    bpm_int = math.floor(bpm)

    ticks_per_beat = mid.ticks_per_beat
    ticks_per_64th = ticks_per_beat / 16

    programs = [0] * 16
    pan_values = [64] * 16
    pitch_bend_values = [0] * 16
    volume_values = [127] * 16
    cc74_values = [0] * 16
    cc75_values = [0] * 16
    cc76_values = [0] * 16
    active_notes = {}
    notes = []

    last_pan = pan_values.copy()
    last_pitch = pitch_bend_values.copy()
    last_vol = volume_values.copy()
    last_prog = programs.copy()
    last_cc74 = cc74_values.copy()
    last_cc75 = cc75_values.copy()
    last_cc76 = cc76_values.copy()

    def sanitize(val):
        return val if val != 0 else -1

    for track in mid.tracks:
        abs_time = 0
        for msg in track:
            abs_time += msg.time

            if msg.type == 'program_change':
                programs[msg.channel] = msg.program

            elif msg.type == 'control_change':
                if msg.control == 10:  # Pan
                    pan_values[msg.channel] = msg.value
                elif msg.control == 7:  # Volume
                    volume_values[msg.channel] = msg.value
                elif msg.control == 74: 
                    cc74_values[msg.channel] = msg.value
                elif msg.control == 75:
                    cc75_values[msg.channel] = msg.value
                elif msg.control == 76:
                    cc76_values[msg.channel] = msg.value

            elif msg.type == 'pitchwheel':
                pitch_bend_values[msg.channel] = msg.pitch

            elif msg.type == 'note_on' and msg.velocity > 0:
                start_div = round(abs_time / ticks_per_64th)
                active_notes[(msg.channel, msg.note)] = (
                    msg.velocity, start_div, programs[msg.channel],
                    pan_values[msg.channel], pitch_bend_values[msg.channel],
                    volume_values[msg.channel], sanitize(cc74_values[msg.channel]),
                    sanitize(cc75_values[msg.channel]), sanitize(cc76_values[msg.channel])
                )

            elif (msg.type == 'note_off') or (msg.type == 'note_on' and msg.velocity == 0):
                key = (msg.channel, msg.note)
                if key in active_notes:
                    vel, start_div, prog, pan, pitch, vol, cc74, cc75, cc76 = active_notes.pop(key)
                    end_div = math.ceil(abs_time / ticks_per_64th)
                    notes.append((msg.channel, prog, msg.note, vel, start_div, end_div,
                                  pan, pitch, vol, cc74, cc75, cc76))

            # Emit dummy line if controller changed but no note
            for ch in range(16):
                if (pan_values[ch] != last_pan[ch] or
                    pitch_bend_values[ch] != last_pitch[ch] or
                    volume_values[ch] != last_vol[ch] or
                    programs[ch] != last_prog[ch] or
                    cc74_values[ch] != last_cc74[ch] or
                    cc75_values[ch] != last_cc75[ch] or
                    cc76_values[ch] != last_cc76[ch]):

                    start_div = round(abs_time / ticks_per_64th)

                    last_vel = None
                    for (ach, anote), (avel, *_rest) in active_notes.items():
                        if ach == ch:
                            last_vel = avel
                            break

                    if last_vel is None:
                        for n in reversed(notes):
                            if n[0] == ch and n[2] != -1:
                                last_vel = n[3]
                                break

                    if last_vel is None:
                        last_vel = 64

                    notes.append((ch, programs[ch], -1, last_vel, start_div, start_div,
                                  pan_values[ch], pitch_bend_values[ch], volume_values[ch],
                                  sanitize(cc74_values[ch]), sanitize(cc75_values[ch]), sanitize(cc76_values[ch])))

                    last_pan[ch] = pan_values[ch]
                    last_pitch[ch] = pitch_bend_values[ch]
                    last_vol[ch] = volume_values[ch]
                    last_prog[ch] = programs[ch]
                    last_cc74[ch] = cc74_values[ch]
                    last_cc75[ch] = cc75_values[ch]
                    last_cc76[ch] = cc76_values[ch]

    notes.sort(key=lambda n: n[4])

    # Write output
    with open(output_path, 'w') as f:
        f.write(f"BPM:{bpm_int}\n")
        for n in notes:
            f.write(f"{n[0]},{n[1]},{n[2]},{n[3]},{n[4]},{n[5]},{n[6]},{n[7]},{n[8]},{n[9]},{n[10]},{n[11]}\n")

    # Remove duplicates
    with open(output_path, 'r') as f:
        lines = f.readlines()

    header = lines[0]
    body = lines[1:]

    seen = set()
    sanitized = []
    for line in body:
        if line not in seen:
            sanitized.append(line)
            seen.add(line)

    with open(output_path, 'w') as f:
        f.write(header)
        f.writelines(sanitized)

    print(f"Converted '{midi_path}' -> '{output_path}' ({len(sanitized)} unique events, BPM={bpm_int})")

if __name__ == "__main__":
    root = tk.Tk()
    root.withdraw()

    midi_path = filedialog.askopenfilename(title="Select MIDI file", filetypes=[("MIDI files", "*.mid *.midi")])
    if not midi_path:
        print("No MIDI file selected.")
        exit()

    output_path = filedialog.asksaveasfilename(title="Save TXT file as", defaultextension=".txt", filetypes=[("Text files", "*.txt")])
    if not output_path:
        print("No output file selected.")
        exit()

    midi_to_txt(midi_path, output_path)
