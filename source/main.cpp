#include <stdio.h>
#include <iostream>
#include <fstream>
#include <cmath>
#include <vector>
#include <sstream>
#include <string>
#include <nds.h>
#include <nds/arm9/sound.h>
#include <filesystem.h>
#include <nf_lib.h>
#include <fat.h>
#include <map>

#define OCTAVE_SHIFT 36
#define PSG_OFFSET 0
#define PITCH_BEND_RANGE_SEMITONES 12.0f
#define GLOBAL_VOLUME_MULTIPLIER 0.5f
#define MAX_DRUM_NOTES 128  // Support all MIDI notes as potential triggers

// Note struct with per-note paramters that is then sent to each channel
struct Note {
    int channel, program, noteNumber, velocity, startDiv, endDiv, startDivFrames, endDivFrames;
    int pan;
    int pitchBend;
    int channelVolume;
    int cc74; // ADSR envelope index (0-based, -1 = none)
    int cc75; // Pitch/mod envelope index (0-based, -1 = none)
    int cc76; // Slide envelope (0-based, -1 = none)
};

// ADSR; attack, decay, and release are measured in 64ths, while sustain is measured in level from 0-127
struct VolumeEnv {
    int A, D, S, R; // Attack, Decay, Sustain, Release (64ths)
};

// Struct for tracking the current state of the *volume* envelope
struct NoteState {
    int envPhase = 0;   // 0=inactive,1=Attack,2=Decay,3=Sustain,4=Release
    float amp = 0;      // Current amplitude 0–127
    int envCounter = 0; // Counter for current phase
};

// Envelope for pitch
struct PitchEnv {
    int delay;  // in 64ths
    int rate;   // in 64ths (period)
    float depth;// in Hz
    int ramp;   // in 64ths (fade-in)
};

// Struct for tracking the state of the *pitch* envelope
struct PitchState {
    bool active = false;
    int phaseState = 0; 
    float phase = 0.0f;
    float currDepth = 0.0f;
    int counter = 0;
};

// Struct for tracking the state of the *slide* envelope
struct SlideEnv {
    int startNote[MAX_DRUM_NOTES];   // indexed by trigger note
    int endNote[MAX_DRUM_NOTES];
    int duration64[MAX_DRUM_NOTES];
    bool defined[MAX_DRUM_NOTES];    // track which notes are defined
    bool isRelative[MAX_DRUM_NOTES];
};

struct SlideState {
    bool active = false;
    int startFrame = 0;
    int startNote = 0;
    int endNote = 0;
    int counter = 0;       // counts frames or 64th ticks
    int duration64 = 0;    // duration of the slide
};

// Unused until later (maybe)
enum bitDepth { form8bit = 0, form16bit = 1, formADPCM = 2 };

bool fileExists(const std::string& path) {
    std::ifstream file(path);
    return file.is_open();
}

// Converts MIDI note + pitch bend to frequency in Hz
float midiNoteToHz(int note, int pitchBend = 0) {
    float semitoneOffset = (pitchBend / 8192.0f) * PITCH_BEND_RANGE_SEMITONES;
    return 440.0f * pow(2.0f, ((note + OCTAVE_SHIFT + semitoneOffset) - 69) / 12.0f);
}

// Load notes from TXT file
std::vector<Note> loadNotes(const std::string& path, int& BPM) {
    std::ifstream file(path);
    std::string line;
    std::vector<Note> notes;

    if (!file.is_open()) return notes;

    if (std::getline(file, line) && line.find("BPM:") == 0)
        BPM = std::stoi(line.substr(4));

    while (std::getline(file, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string token;
        Note n;

        // Put the values into the note array
        std::getline(ss, token, ','); n.channel = std::stoi(token);
        std::getline(ss, token, ','); n.program = std::stoi(token);
        std::getline(ss, token, ','); n.noteNumber = std::stoi(token);
        std::getline(ss, token, ','); n.velocity = std::stoi(token);
        std::getline(ss, token, ','); n.startDiv = std::stoi(token);
        std::getline(ss, token, ','); n.endDiv = std::stoi(token);
        std::getline(ss, token, ','); n.pan = std::stoi(token);
        std::getline(ss, token, ','); n.pitchBend = std::stoi(token);
        std::getline(ss, token, ','); n.channelVolume = std::stoi(token);

        std::getline(ss, token, ',');
        n.cc74 = std::stoi(token); if (n.cc74 > 0) n.cc74 -= 1; else n.cc74 = -1;
        std::getline(ss, token, ',');
        n.cc75 = std::stoi(token); if (n.cc75 > 0) n.cc75 -= 1; else n.cc75 = -1;
        std::getline(ss, token, ',');
        n.cc76 = std::stoi(token); if (n.cc76 > 0) n.cc76 -= 1; else n.cc76 = -1;

        // Skip loop marker notes (C0 = MIDI 0, C#1 = MIDI 1)
        if (n.noteNumber == 0 || n.noteNumber == 1) continue;

        notes.push_back(n);
    }

    return notes;
}

// Load volume envelopes
void loadVolumeEnvelopes(const std::string& path, VolumeEnv volEnvelopes[16]) {
    std::ifstream file(path);
    if (!file.is_open()) { std::cout << "Failed to open envelope file: " << path << std::endl; return; }

    auto trim = [](std::string s) -> std::string {
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    };

    std::string line;
    int loaded = 0;

    while (std::getline(file, line)) {
        if (line.empty()) continue;
        size_t pos = line.find("Volume_Env");
        if (pos == std::string::npos) continue;
        size_t colon = line.find(':', pos);
        if (colon == std::string::npos) continue;

        std::string label = trim(line.substr(pos, colon - pos));
        std::string values = trim(line.substr(colon + 1));

        int envIndex = -1;
        size_t prefixLen = 10;
        if (label.length() > prefixLen) {
            std::string numStr = trim(label.substr(prefixLen));
            if (!numStr.empty() && isdigit(numStr[0])) {
                int n = std::atoi(numStr.c_str());
                envIndex = n - 1;
            }
        }

        if (envIndex < 0 || envIndex >= 16) continue;

        std::stringstream valStream(values);
        std::string token;
        int envValues[4] = {0,0,0,0};
        int i = 0;
        while (std::getline(valStream, token, ',') && i < 4)
            envValues[i++] = std::atoi(trim(token).c_str());

        volEnvelopes[envIndex] = { envValues[0], envValues[1], envValues[2], envValues[3] };
        loaded++;
    }

    std::cout << "Loaded " << loaded << " volume envelopes." << std::endl;
}

// Load pitch envelopes
void loadPitchEnvelopes(const std::string& path, PitchEnv pitchEnvelopes[16]) {
    std::ifstream file(path);
    if (!file.is_open()) { std::cout << "Failed to open pitch envelope file: " << path << std::endl; return; }

    auto trim = [](std::string s) -> std::string {
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    };

    std::string line;
    int loaded = 0;

    while (std::getline(file, line)) {
        if (line.empty()) continue;
        size_t pos = line.find("Pitch_Env");
        if (pos == std::string::npos) continue;
        size_t colon = line.find(':', pos);
        if (colon == std::string::npos) continue;

        std::string label = trim(line.substr(pos, colon - pos));
        std::string values = trim(line.substr(colon + 1));

        int envIndex = -1;
        size_t prefixLen = 9;
        if (label.length() > prefixLen) {
            std::string numStr = trim(label.substr(prefixLen));
            if (!numStr.empty() && isdigit(numStr[0])) {
                int n = std::atoi(numStr.c_str());
                envIndex = n - 1;
            }
        }

        if (envIndex < 0 || envIndex >= 16) continue;

        std::stringstream valStream(values);
        std::string token;
        int delay=0, rate=1, ramp=0; float depth=0.0f;
        if (std::getline(valStream, token, ',')) delay = std::atoi(trim(token).c_str());
        if (std::getline(valStream, token, ',')) rate  = std::atoi(trim(token).c_str());
        if (std::getline(valStream, token, ',')) depth = std::atof(trim(token).c_str());
        if (std::getline(valStream, token, ',')) ramp  = std::atoi(trim(token).c_str());

        pitchEnvelopes[envIndex] = { delay, rate, depth, ramp };
        loaded++;
    }

    std::cout << "Loaded " << loaded << " pitch envelopes." << std::endl;
}

void loadSlideEnvelopes(const std::string& path, SlideEnv slideEnvs[16]){
    std::ifstream file(path); 
    if(!file.is_open()) return;
    
    auto trim = [](std::string s){
        size_t start=s.find_first_not_of(" \t\r\n"); 
        if(start==std::string::npos) return std::string("");
        size_t end=s.find_last_not_of(" \t\r\n"); 
        return s.substr(start,end-start+1);
    };
    
    // Initialize all as undefined
    for(int i = 0; i < 16; i++) {
        for(int n = 0; n < MAX_DRUM_NOTES; n++) {
            slideEnvs[i].defined[n] = false;
            slideEnvs[i].startNote[n] = 0;
            slideEnvs[i].endNote[n] = 0;
            slideEnvs[i].duration64[n] = 0;
            slideEnvs[i].isRelative[n] = false;
        }
    }
    
    std::string line;
    while(std::getline(file,line)){
        if(line.empty()) continue;
        size_t pos = line.find("Slide_Env");
        if(pos == std::string::npos) continue;
        size_t colon = line.find(':', pos);
        if(colon == std::string::npos) continue;

        std::string values = trim(line.substr(colon+1));
        
        std::stringstream ss(values); 
        std::string token;
        int kitNum=0, trigger=0, start=0, end=0, dur=0;
        
        if(std::getline(ss,token,',')) kitNum = std::atoi(trim(token).c_str());
        if(std::getline(ss,token,',')) trigger = std::atoi(trim(token).c_str());
        if(std::getline(ss,token,',')) start = std::atoi(trim(token).c_str());
        if(std::getline(ss,token,',')) end = std::atoi(trim(token).c_str());
        if(std::getline(ss,token,',')) dur = std::atoi(trim(token).c_str());
        
        // Convert 1-based kit number to 0-based index
        int kitIdx = kitNum - 1;
        if(kitIdx < 0 || kitIdx >= 16) continue;
        
        if(trigger == -1) {
            // Relative mode: apply to ALL notes
            // Pre-calculate duration per semitone from absolute duration
            int distance = abs(end - start);
            int durationPerSemitone = (distance > 0) ? dur : 0;  // Store as absolute, will divide later
            
            for(int n = 0; n < MAX_DRUM_NOTES; n++) {
                slideEnvs[kitIdx].startNote[n] = start;
                slideEnvs[kitIdx].endNote[n] = end;
                slideEnvs[kitIdx].duration64[n] = dur;  // Store absolute duration from file
                slideEnvs[kitIdx].defined[n] = true;
                slideEnvs[kitIdx].isRelative[n] = true;
            }
        } else {
            // Normal mode: specific trigger note
            if(trigger < 0 || trigger >= MAX_DRUM_NOTES) continue;
            slideEnvs[kitIdx].startNote[trigger] = start;
            slideEnvs[kitIdx].endNote[trigger] = end;
            slideEnvs[kitIdx].duration64[trigger] = dur;
            slideEnvs[kitIdx].defined[trigger] = true;
            slideEnvs[kitIdx].isRelative[trigger] = false;
        }
    }
}

// DS initialization
void initDS() {
    NF_Set2D(0, 0);
    consoleDemoInit();
    printf("\n NitroFS init. Please wait.\n\n");
    swiWaitForVBlank();

    fatInitDefault();
    NF_SetRootFolder("fat:/NuclearSEQ");
    soundEnable();
    NF_InitTiledBgBuffers();
    NF_InitTiledBgSys(0);
    NF_InitRawSoundBuffers();
    NF_LoadTiledBg("bg/layer3", "moon", 256, 256);
    NF_LoadTextFont("fnt/default", "normal", 256, 256, 0);
    NF_CreateTiledBg(0, 3, "moon");
}

int main(int argc, char *argv[]) {
    initDS();

    std::string songName = "demoSong.txt";

    int BPM = 120;
    if (fileExists("fat:/NuclearSEQ/seq/song.txt")) {
        songName = "song.txt";
    } 
    
    std::vector<Note> notes = loadNotes("fat:/NuclearSEQ/seq/" + songName, BPM);

    // Detect loop markers (C0 = MIDI 0 start, C#1 = MIDI 1 end)
    int loopStart64th = -1;
    int loopEnd64th = -1;
    {
        std::ifstream loopFile("fat:/NuclearSEQ/seq/" + songName);
        std::string line;
        if (std::getline(loopFile, line) && line.find("BPM:") == 0) {} // skip BPM
        while (std::getline(loopFile, line)) {
            if (line.empty()) continue;
            std::stringstream ss(line);
            std::string token;
            int channel, program, noteNum, velocity, startDiv, endDiv, pan, pitchBend, vol;
            std::getline(ss, token, ','); channel = std::stoi(token);
            std::getline(ss, token, ','); program = std::stoi(token);
            std::getline(ss, token, ','); noteNum = std::stoi(token);
            std::getline(ss, token, ','); velocity = std::stoi(token);
            std::getline(ss, token, ','); startDiv = std::stoi(token);
            if (noteNum == 0 && loopStart64th == -1) loopStart64th = startDiv;
            if (noteNum == 1 && loopEnd64th == -1) loopEnd64th = startDiv;
        }
    }

    float framesPer64th = (60.0f / BPM) / 16.0f * 59.73f; // 64th-note timing
    for (Note& n : notes) {
        n.startDivFrames = static_cast<int>(round(n.startDiv * framesPer64th));
        n.endDivFrames   = static_cast<int>(round(n.endDiv * framesPer64th));
    }

    if (loopStart64th != -1)
        loopStart64th = static_cast<int>(round(loopStart64th * framesPer64th));
    if (loopEnd64th != -1)
        loopEnd64th = static_cast<int>(round(loopEnd64th * framesPer64th));

    if (loopStart64th != -1 && loopEnd64th != -1 && loopEnd64th > loopStart64th)
        std::cout << "Loop points set: " << (loopStart64th / framesPer64th) << " → " << (loopEnd64th / framesPer64th) << std::endl;
    else
        std::cout << "No valid loop points found." << std::endl;

    // Create arrays for channel states
    int currentNotePlaying[16], currentPitchBend[16], currentPan[16], currentVolume[16], currentNoteProgram[16];
    int currentCC74[16], currentCC75[16];
    bool channelActive[16];
    VolumeEnv volEnvelopes[16];
    NoteState noteStates[16];
    int noteBaseVolume[16];
    PitchEnv pitchEnvelopes[16];
    PitchState pitchStates[16];
    SlideEnv slideEnvelopes[16];
    SlideState slideStates[16];


    for (int i = 0; i < 16; i++){
        currentNotePlaying[i] = 60; currentPitchBend[i] = 0; currentPan[i] = 64;
        currentVolume[i] = 127; currentNoteProgram[i] = 0;
        currentCC74[i] = -1; currentCC75[i] = -1;
        channelActive[i] = false;
        noteStates[i] = {}; 
        pitchStates[i] = {};
        volEnvelopes[i] = {4,8,100,12};
        pitchEnvelopes[i] = {0,1,0.0f,0};
        noteBaseVolume[i] = 127;
        slideStates[i] = {};
    }

    loadVolumeEnvelopes("fat:/NuclearSEQ/seq/envelopes.txt", volEnvelopes);
    loadPitchEnvelopes("fat:/NuclearSEQ/seq/envelopes.txt", pitchEnvelopes);
    loadSlideEnvelopes("fat:/NuclearSEQ/seq/envelopes.txt", slideEnvelopes);

    consoleClear();

    int frameMod = 0;

    while (1) {
        // Opening screen
        while (1) {
            consoleClear();
            std::cout << "Welcome to the NuclearSEQ player.\n";
            std::cout << "The song that will be played\n should be put in NuclearSEQ/seq/song.txt\n" << "Otherwise demoSong.txt will be \nplayed instead." << std::endl;
            std::cout << "Press any button to continue." << std::endl;
            scanKeys(); 
            uint16_t keys = keysDown();

            if (keys) {
                std::cout << "Button pressed - entering playback loop." << std::endl; // debug: menu state change
                break;
            }
            swiWaitForVBlank();
        }

        while (1) {
            frameMod++;
            int current64th = frameMod / framesPer64th;
            
            // Process note events
            for (Note& n : notes) {
                if (frameMod == n.startDivFrames) {
                    if (n.noteNumber != -1) {
                        // Note-on
                        int finalVol = (n.velocity * n.channelVolume) / 127;
                        finalVol = (int)(finalVol * GLOBAL_VOLUME_MULTIPLIER);
                        if (finalVol > 127) finalVol = 127; 
                        if (finalVol < 0) finalVol = 0;
                        noteBaseVolume[n.channel] = finalVol;
                        float freq = midiNoteToHz(n.noteNumber, n.pitchBend);

                        if (n.channel >= 14) {
                            // Channels 15-16 (0-indexed 14-15) play noise
                            soundPlayNoiseChannel(n.channel + PSG_OFFSET, freq, finalVol, n.pan);
                        } else {
                            DutyCycle duty = static_cast<DutyCycle>(n.program);
                            soundPlayPSGChannel(n.channel + PSG_OFFSET, duty, freq, finalVol, n.pan);
                        }


                        currentNotePlaying[n.channel] = n.noteNumber;
                        currentPitchBend[n.channel] = n.pitchBend;
                        currentPan[n.channel] = n.pan;
                        currentVolume[n.channel] = n.channelVolume;
                        currentNoteProgram[n.channel] = n.program;
                        currentCC74[n.channel] = n.cc74;
                        currentCC75[n.channel] = n.cc75;
                        channelActive[n.channel] = true;

                        if (n.cc74 != -1) {
                            noteStates[n.channel].envPhase = 1;
                            noteStates[n.channel].envCounter = 0;
                            noteStates[n.channel].amp = 0;
                        }

                        if (n.cc75 != -1) {
                            PitchEnv &penv = pitchEnvelopes[n.cc75];
                            PitchState &pstate = pitchStates[n.channel];
                            pstate.active = true; pstate.phaseState = 0;
                            pstate.phase = 0.0f; pstate.currDepth = 0.0f; pstate.counter = 0;
                        }

                        if(n.cc76 != -1 && n.cc76 < 16) {
                            SlideEnv &se = slideEnvelopes[n.cc76];
                            
                            if (n.noteNumber >= 0 && n.noteNumber < MAX_DRUM_NOTES && se.defined[n.noteNumber]) {
                                SlideState &ss = slideStates[n.channel];
                                ss.active = true;
                                
                                int startNote = se.startNote[n.noteNumber];
                                int endNote = se.endNote[n.noteNumber];
                                int duration = se.duration64[n.noteNumber];
                                
                                // If relative mode, offset from the played note
                                if(se.isRelative[n.noteNumber]) {
                                    startNote = n.noteNumber + startNote;  // Offset from played note
                                    endNote = n.noteNumber + endNote;      // Offset from played note
                                }
                                
                                ss.startNote = startNote;
                                ss.endNote = endNote;
                                ss.startFrame = frameMod;
                                ss.duration64 = duration;
                            }
                        }

                    } else {
                        // Controller-only event
                        currentPitchBend[n.channel] = n.pitchBend;
                        currentPan[n.channel] = n.pan;
                        currentVolume[n.channel] = n.channelVolume;
                        currentNoteProgram[n.channel] = n.program;
                        currentCC74[n.channel] = n.cc74;
                        currentCC75[n.channel] = n.cc75;
                        soundSetPan(n.channel+PSG_OFFSET, n.pan);
                        int finalVol = noteBaseVolume[n.channel];
                        soundSetVolume(n.channel+PSG_OFFSET, finalVol);
                    }
                }

                if (frameMod == n.endDivFrames && n.noteNumber != -1) {
                    if (n.cc74 == -1) {
                        soundKill(n.channel + PSG_OFFSET); channelActive[n.channel] = false;
                    } else {
                        noteStates[n.channel].envPhase = 4; noteStates[n.channel].envCounter = 0;
                    }
                    if (n.cc75 != -1) {
                        PitchState &pstate = pitchStates[n.channel];
                        if (pstate.active) {
                            pstate.active = false; pstate.phase = 0; pstate.currDepth=0; pstate.counter=0;
                            if (channelActive[n.channel]) {
                                float baseFreq = midiNoteToHz(currentNotePlaying[n.channel], currentPitchBend[n.channel]);
                                soundSetFreq(n.channel+PSG_OFFSET, baseFreq);
                            }
                        }
                    }
                }
            }

            // Update ADSR
            for (int ch = 0; ch < 16; ch++) {
                if (!channelActive[ch]) continue;
                int envIndex = currentCC74[ch];
                if (envIndex < 0 || envIndex >= 16) continue;

                VolumeEnv &env = volEnvelopes[envIndex];
                NoteState &state = noteStates[ch];
                int baseVol = noteBaseVolume[ch];
                state.envCounter++;

                switch (state.envPhase) {
                    case 1:
                        state.amp += (127.0f / std::max(1, env.A)) / 1.0f;
                        if (state.amp >= 127.0f) { state.amp = 127.0f; state.envPhase = 2; state.envCounter=0; }
                        break;
                    case 2:
                        state.amp -= ((127.0f - env.S) / std::max(1, env.D));
                        if (state.amp <= env.S) { state.amp = env.S; state.envPhase = 3; }
                        break;
                    case 4:
                        state.amp -= (env.S / std::max(1, env.R));
                        if (state.amp <= 0) { state.amp = 0; state.envPhase=0; soundKill(ch+PSG_OFFSET); channelActive[ch]=false; }
                        break;
                }

                int vol = (int)(state.amp / 127.0f * baseVol);
                if (vol < 0) vol = 0; if (vol > 127) vol = 127;
                soundSetVolume(ch + PSG_OFFSET, vol);
            }

            // Update pitch envelopes
            for (int ch = 0; ch < 16; ch++) {
                if (!channelActive[ch]) continue;
                if (slideStates[ch].active) continue;

                PitchState &pstate = pitchStates[ch];
                int penvIndex = currentCC75[ch];
                if (!pstate.active || penvIndex < 0 || penvIndex >= 16) continue;

                PitchEnv &penv = pitchEnvelopes[penvIndex];
                pstate.counter++;
                if (pstate.phaseState == 0 && pstate.counter >= penv.delay) {
                    pstate.phaseState = 1; pstate.counter = 0;
                }
                if (pstate.phaseState == 1) {
                    if (pstate.currDepth < penv.depth) {
                        pstate.currDepth += (penv.depth / std::max(1, penv.ramp));
                        if (pstate.currDepth > penv.depth) pstate.currDepth = penv.depth;
                    }
                    pstate.phase += (2.0f * M_PI / std::max(1, penv.rate));
                    float pitchOffsetHz = sin(pstate.phase) * pstate.currDepth;
                    float baseFreq = midiNoteToHz(currentNotePlaying[ch], currentPitchBend[ch]);
                    float newFreq = baseFreq + pitchOffsetHz;
                    if (newFreq < 20.0f) newFreq = 20.0f;
                    if (newFreq > 20000.0f) newFreq = 20000.0f;
                    soundSetFreq(ch + PSG_OFFSET, newFreq);
                }
            }

            for(int ch = 0; ch < 16; ch++) {
                SlideState &ss = slideStates[ch];
                if(ss.active && ss.duration64 > 0){
                    int elapsedFrames = frameMod - ss.startFrame;
                    float elapsed64ths = elapsedFrames / framesPer64th;
                    float t = elapsed64ths / ss.duration64;
                    
                    if(t >= 1.0f) { 
                        t = 1.0f; 
                        ss.active = false; 
                    }

                    float currentNote = ss.startNote + t * (ss.endNote - ss.startNote);
                    float freq = midiNoteToHz((int)currentNote, currentPitchBend[ch]);

                    // Clamp to u16 range
                    if (freq < 0.0f) freq = 0.0f;
                    if (freq > 65535.0f) freq = 65535.0f;
                    
                    soundSetFreq(ch + PSG_OFFSET, (u16)freq);
                }
            }


            // Debug print (per-channel CC/envelope and global info)
            consoleClear();
            for (int ch = 0; ch < 16; ch++) {
                std::cout << "Ch " << ch << " CC74:" << currentCC74[ch] << " CC75:" << currentCC75[ch];
                if (currentCC74[ch] != -1) {
                    VolumeEnv &e = volEnvelopes[currentCC74[ch]];
                    std::cout << " [A:" << e.A << " D:" << e.D << " S:" << e.S << " R:" << e.R << "]";
                }
                if (currentCC75[ch] != -1) {
                    PitchEnv &p = pitchEnvelopes[currentCC75[ch]];
                    std::cout << " (P delay:" << p.delay << " rate:" << p.rate << " depth:" << p.depth << " ramp:" << p.ramp << ")";
                }
                std::cout << std::endl;
            }
            const std::string testString = "fat:/NuclearSEQ/seq/" + songName;

            std::cout << "64th:" << current64th << " BPM:" << BPM << " framesPer64th:" << framesPer64th << " frameMod:" << frameMod << std::endl;
            std::cout << testString << std::endl;

            // Handle looping
            if (loopStart64th != -1 && loopEnd64th != -1 && frameMod >= loopEnd64th) {
                frameMod = (loopStart64th > 0) ? loopStart64th - 1 : 0;

                // Reset slide states
                for (int ch = 0; ch < 16; ch++) {
                    slideStates[ch].active = false;
                }

                /*
                // Reset states
                for (int ch = 0; ch < 16; ch++) {
                    if (channelActive[ch]) {
                        soundKill(ch + PSG_OFFSET);
                        channelActive[ch] = false;
                        noteStates[ch].envPhase = 0;
                        noteStates[ch].amp = 0;
                        pitchStates[ch].active = false;
                        pitchStates[ch].phase = 0;
                    }
                }
                */

                consoleClear();
                std::cout << "Looping back to 64th: " << (loopStart64th / framesPer64th) << std::endl;
            }

            swiWaitForVBlank();
        }
    }

    return 0;
}
