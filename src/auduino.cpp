// Auduino, the Lo-Fi granular synthesiser
//
// by Peter Knight, Tinker.it http://tinker.it
//
// Help:      http://code.google.com/p/tinkerit/wiki/Auduino
// More help: http://groups.google.com/group/auduino
//
// Analog in 0: Grain 1 pitch
// Analog in 1: Grain 2 decay
// Analog in 2: Grain 1 decay
// Analog in 3: Grain 2 pitch
// Analog in 4: Grain repetition frequency
//
// Digital 3: Audio out (Digital 11 on ATmega8)
//
// Changelog:
// 19 Nov 2008: Added support for ATmega8 boards
// 21 Mar 2009: Added support for ATmega328 boards
// 7  Apr 2009: Fixed interrupt vector for ATmega328 boards
// 8  Apr 2009: Added support for ATmega1280 boards (Arduino Mega)
// 12 Oct 2012: Made source more C++11 friendly, added initial Midi

#include <Arduino.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include "phase.h"
#include "grain.h"
#include "midi.h"
#include "asm.h"
#include "debug.h"

struct Note {
  enum Gate {
    CLOSED,
    OPEN,
  } gate;

  uint8_t number;
  uint8_t velocity;
};

static struct Voice {
  Note note;
  Env env;
  Phase sync[2];
  Grain grains[2];
} voices[2];

// Map Analogue channels
#define SYNC_CONTROL         (4)
#define GRAIN_FREQ_CONTROL   (0)
#define GRAIN_DECAY_CONTROL  (2)
#define GRAIN2_FREQ_CONTROL  (3)
#define GRAIN2_DECAY_CONTROL (1)


// Changing these will also requires rewriting audioOn()

#if defined(__AVR_ATmega8__)
//
// On old ATmega8 boards.
//    Output is on pin 11
//
#define LED_PIN       13
#define LED_PORT      PORTB
#define LED_BIT       5
#define PWM_PIN       11
#define PWM_VALUE     OCR2
#define PWM_INTERRUPT TIMER2_OVF_vect
#elif defined(__AVR_ATmega1280__)
//
// On the Arduino Mega
//    Output is on pin 3
//
#define LED_PIN       13
#define LED_PORT      PORTB
#define LED_BIT       7
#define PWM_PIN       3
#define PWM_VALUE     OCR3C
#define PWM_INTERRUPT TIMER3_OVF_vect
#else
//
// For modern ATmega168 and ATmega328 boards
//    Output is on pin 3
//
#define PWM_PIN       3
#define PWM_VALUE     OCR2B
#define LED_PIN       13
#define LED_PORT      PORTB
#define LED_BIT       5
#define PWM_INTERRUPT TIMER2_OVF_vect
#endif

// Smooth logarithmic mapping
//
static const uint16_t antilogTable[64] PROGMEM = {
  64830,64132,63441,62757,62081,61413,60751,60097,59449,58809,58176,57549,56929,56316,55709,55109,
  54515,53928,53347,52773,52204,51642,51085,50535,49991,49452,48920,48393,47871,47356,46846,46341,
  45842,45348,44859,44376,43898,43425,42958,42495,42037,41584,41136,40693,40255,39821,39392,38968,
  38548,38133,37722,37316,36914,36516,36123,35734,35349,34968,34591,34219,33850,33486,33125,32768
};

static uint16_t mapPhaseInc(uint16_t input) {
  return (pgm_read_word(&antilogTable[input & 0x3f])) >> (input >> 6);
}

#include <math.h>

constexpr double midiNoteToFreq(double p) {
  return pow(2.0, (p - 69) / 12.0) * 440.0;
}

constexpr uint16_t freqToInc(double f, double accSteps, double sr) {
  // with rounding
  return f * accSteps / sr + 0.5;
}

#define MIDI_TO_INC(p) freqToInc(midiNoteToFreq(p), 65536, 31250)

// Stepped chromatic mapping
//
static const uint16_t midiTable[128] PROGMEM = {
  MIDI_TO_INC(0), MIDI_TO_INC(1), MIDI_TO_INC(2), MIDI_TO_INC(3), MIDI_TO_INC(4), MIDI_TO_INC(5), MIDI_TO_INC(6), MIDI_TO_INC(7),
  MIDI_TO_INC(8), MIDI_TO_INC(9), MIDI_TO_INC(10), MIDI_TO_INC(11), MIDI_TO_INC(12), MIDI_TO_INC(13), MIDI_TO_INC(14), MIDI_TO_INC(15),
  MIDI_TO_INC(16), MIDI_TO_INC(17), MIDI_TO_INC(18), MIDI_TO_INC(19), MIDI_TO_INC(20), MIDI_TO_INC(21), MIDI_TO_INC(22), MIDI_TO_INC(23),
  MIDI_TO_INC(24), MIDI_TO_INC(25), MIDI_TO_INC(26), MIDI_TO_INC(27), MIDI_TO_INC(28), MIDI_TO_INC(29), MIDI_TO_INC(30), MIDI_TO_INC(31),
  MIDI_TO_INC(32), MIDI_TO_INC(33), MIDI_TO_INC(34), MIDI_TO_INC(35), MIDI_TO_INC(36), MIDI_TO_INC(37), MIDI_TO_INC(38), MIDI_TO_INC(39),
  MIDI_TO_INC(40), MIDI_TO_INC(41), MIDI_TO_INC(42), MIDI_TO_INC(43), MIDI_TO_INC(44), MIDI_TO_INC(45), MIDI_TO_INC(46), MIDI_TO_INC(47),
  MIDI_TO_INC(48), MIDI_TO_INC(49), MIDI_TO_INC(50), MIDI_TO_INC(51), MIDI_TO_INC(52), MIDI_TO_INC(53), MIDI_TO_INC(54), MIDI_TO_INC(55),
  MIDI_TO_INC(56), MIDI_TO_INC(57), MIDI_TO_INC(58), MIDI_TO_INC(59), MIDI_TO_INC(60), MIDI_TO_INC(61), MIDI_TO_INC(62), MIDI_TO_INC(63),
  MIDI_TO_INC(64), MIDI_TO_INC(65), MIDI_TO_INC(66), MIDI_TO_INC(67), MIDI_TO_INC(68), MIDI_TO_INC(69), MIDI_TO_INC(70), MIDI_TO_INC(71),
  MIDI_TO_INC(72), MIDI_TO_INC(73), MIDI_TO_INC(74), MIDI_TO_INC(75), MIDI_TO_INC(76), MIDI_TO_INC(77), MIDI_TO_INC(78), MIDI_TO_INC(79),
  MIDI_TO_INC(80), MIDI_TO_INC(81), MIDI_TO_INC(82), MIDI_TO_INC(83), MIDI_TO_INC(84), MIDI_TO_INC(85), MIDI_TO_INC(86), MIDI_TO_INC(87),
  MIDI_TO_INC(88), MIDI_TO_INC(89), MIDI_TO_INC(90), MIDI_TO_INC(91), MIDI_TO_INC(92), MIDI_TO_INC(93), MIDI_TO_INC(94), MIDI_TO_INC(95),
  MIDI_TO_INC(96), MIDI_TO_INC(97), MIDI_TO_INC(98), MIDI_TO_INC(99), MIDI_TO_INC(100), MIDI_TO_INC(101), MIDI_TO_INC(102), MIDI_TO_INC(103),
  MIDI_TO_INC(104), MIDI_TO_INC(105), MIDI_TO_INC(106), MIDI_TO_INC(107), MIDI_TO_INC(108), MIDI_TO_INC(109), MIDI_TO_INC(110), MIDI_TO_INC(111),
  MIDI_TO_INC(112), MIDI_TO_INC(113), MIDI_TO_INC(114), MIDI_TO_INC(115), MIDI_TO_INC(116), MIDI_TO_INC(117), MIDI_TO_INC(118), MIDI_TO_INC(119),
  MIDI_TO_INC(120), MIDI_TO_INC(121), MIDI_TO_INC(122), MIDI_TO_INC(123), MIDI_TO_INC(124), MIDI_TO_INC(125), MIDI_TO_INC(126), MIDI_TO_INC(127),
};
/*
  17,18,19,20,22,23,24,26,27,29,31,32,34,36,38,41,43,46,48,51,54,58,61,65,69,73,
  77,82,86,92,97,103,109,115,122,129,137,145,154,163,173,183,194,206,218,231,
  244,259,274,291,308,326,346,366,388,411,435,461,489,518,549,581,616,652,691,
  732,776,822,871,923,978,1036,1097,1163,1232,1305,1383,1465,1552,1644,1742,
  1845,1955,2071,2195,2325,2463,2610,2765,2930,3104,3288,3484,3691,3910,4143,
  4389,4650,4927,5220,5530,5859,6207,6577,6968,7382,7821,8286,8779,9301,9854,
  10440,11060,11718,12415,13153,13935,14764,15642,16572,17557,18601,19708,20879,
  22121,23436,24830,26306
};
*/

static uint16_t mapMidi(uint16_t input) {
  return pgm_read_word(&midiTable[(1023-input) >> 3]);
}

// Stepped Pentatonic mapping
//
static const uint16_t pentatonicTable[54] PROGMEM = {
  0,19,22,26,29,32,38,43,51,58,65,77,86,103,115,129,154,173,206,231,259,308,346,
  411,461,518,616,691,822,923,1036,1232,1383,1644,1845,2071,2463,2765,3288,
  3691,4143,4927,5530,6577,7382,8286,9854,11060,13153,14764,16572,19708,22121,26306
};

static uint16_t mapPentatonic(uint16_t input) {
  uint8_t value = (1023-input) * 53 >> 10; // ~= / (53/1024)
  return pgm_read_word(&pentatonicTable[value]);
}


static void audioOn() {
#if defined(__AVR_ATmega8__)
  // ATmega8 has different registers
  TCCR2 = _BV(WGM20) | _BV(COM21) | _BV(CS20);
  TIMSK = _BV(TOIE2);
#elif defined(__AVR_ATmega1280__)
  TCCR3A = _BV(COM3C1) | _BV(WGM30);
  TCCR3B = _BV(CS30);
  TIMSK3 = _BV(TOIE3);
#else
  // Set up PWM to 31.25kHz, phase accurate
  TCCR2A = _BV(COM2B1) | _BV(WGM20);
  TCCR2B = _BV(CS20);
  TIMSK2 = _BV(TOIE2);
#endif
}


void setup() {
  SETUP_DEBUG();
  pinMode(PWM_PIN,OUTPUT);
  audioOn();
  pinMode(LED_PIN,OUTPUT);
  // setup midi
  Midi.begin();
  // set handlers
  Midi.handlers.noteOn = [] (MidiMessage &message) {
    // no bounds checking, midi should not produce
    // note values higher than 127
    uint8_t number = message.data[0];
    uint8_t velocity = message.data[1];

    if (velocity) {
      voices[0].note.number = number;
      voices[0].note.velocity = velocity;
      voices[0].note.gate = Note::OPEN;

      voices[0].env.amp = velocity << 8;
      voices[0].env.decay = 1;
      voices[0].env.divider = 4;

      voices[0].sync[0].setInc(pgm_read_word(&midiTable[voices[0].note.number - 24]));
      voices[0].sync[1].setInc(pgm_read_word(&midiTable[voices[0].note.number - 17]));
    } else if (voices[0].note.number == number) {
      voices[0].note.gate = Note::CLOSED;
    }
  };
  Midi.handlers.noteOff = [] (MidiMessage &message) {
    if (voices[0].note.number == message.data[0]) {
      voices[0].note.gate = Note::CLOSED;
    }
  };
  Midi.handlers.controlChange = [] (MidiMessage &message) {
    uint8_t controller = message.data[0];
    uint8_t value = message.data[1];

    switch (controller) {
      case 1:
        // mod wheel
        voices[0].grains[0].env.decay = value >> 3;
        voices[0].grains[1].env.decay = value >> 4;
        break;
      case 16: voices[0].grains[0].phase.setInc(pgm_read_word(&midiTable[value])); break;
      //case 21: grains[0].env.decay = value << 1; break;
      case 17: voices[0].grains[1].phase.setInc(pgm_read_word(&midiTable[value])); break;
      //case 23: grains[1].env.decay = value; break;
    }
  };
  Midi.handlers.pitchWheelChange = [] (MidiMessage &message) {
    // 14bit
    uint16_t value = message.data[1] << 7 | message.data[0];
    voices[0].sync[0].modulate(value);
    voices[0].sync[1].modulate(value);
  };
}

void loop() {
  // The loop is pretty simple - it just updates the parameters for the oscillators.
  //
  // Avoid using any functions that make extensive use of interrupts, or turn interrupts off.
  // They will cause clicks and poops in the audio.
 
  // Smooth frequency mapping
  //syncPhaseInc = mapPhaseInc(analogRead(SYNC_CONTROL)) / 4;
 
  // Stepped mapping to MIDI notes: C, Db, D, Eb, E, F...
  //syncPhaseInc = mapMidi(analogRead(SYNC_CONTROL));
 
  // Stepped pentatonic mapping: D, E, G, A, B
  //syncPhase.inc = mapPentatonic(analogRead(SYNC_CONTROL));

  //grains[0].phase.inc = mapPhaseInc(analogRead(GRAIN_FREQ_CONTROL)) / 2;
  //grains[0].env.decay = analogRead(GRAIN_DECAY_CONTROL) / 8;
  //grains[1].phase.inc = mapPhaseInc(analogRead(GRAIN2_FREQ_CONTROL)) / 2;
  //grains[1].env.decay = analogRead(GRAIN2_DECAY_CONTROL) / 4;
}

ISR(PWM_INTERRUPT)
{
  ++voices[0].sync[0];
  ++voices[0].sync[1];

  if (voices[0].sync[0].hasOverflowed()) {
    // Time to start the next grain
    voices[0].grains[0].reset();
    LED_PORT ^= 1 << LED_BIT; // Faster than using digitalWrite
  }

  if (voices[0].sync[1].hasOverflowed()) {
    voices[0].grains[1].reset();
  }
 
  // Increment the phase of the grain oscillators
  ++voices[0].grains[0].phase;
  ++voices[0].grains[1].phase;

  uint16_t output;
  output =  voices[0].grains[0].getSample();
  output += voices[0].grains[1].getSample();

  // Make the grain amplitudes decay by a factor every sample (exponential decay)
  voices[0].grains[0].env.tick();
  voices[0].grains[1].env.tick();

  // It's ok to leave the PWM to what ever value it is when gate closes,
  // since HPF should remove DC voltages.
  if (voices[0].note.gate == Note::CLOSED) {
    voices[0].env.tick();
  }

  // Scale and shift output to the available signed range for amplitude calculations
  int8_t scaled_output = static_cast<uint8_t>(output >> 7) - 128;

  // Output to PWM (this is faster than using analogWrite)
  // 2 * 127 * 255  + 2 * 255  = 65280,  well within unsigned 16bit limits
  // 2 * 127 * -128 + 2 * -128 = -32768, ok
  // 2 * 127 * 127  + 2 * 127  = 32512,  ok
  // value = output * (velocity + 1) / (127 + 1)
  //       = output * (velocity + 1) * 2 / ((127 + 1) * 2)
  //       = output * (2 * velocity + 2) / 256
  //       = (2 * velocity * output + 2 * output) / 256
  //
  // mul() from grain.h, grain.hpp
  int16_t scaled_output_x2 = mulsu(scaled_output, 2);
  scaled_output_x2 += scaled_output_x2 * voices[0].env.value();
  PWM_VALUE = static_cast<uint8_t>(scaled_output_x2 >> 8) + 128;
}
