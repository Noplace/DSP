/*****************************************************************************************************************
* Copyright (c) 2014 Khalid Ali Al-Kooheji                                                                       *
*                                                                                                                *
* Permission is hereby granted, free of charge, to any person obtaining a copy of this software and              *
* associated documentation files (the "Software"), to deal in the Software without restriction, including        *
* without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell        *
* copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the       *
* following conditions:                                                                                          *
*                                                                                                                *
* The above copyright notice and this permission notice shall be included in all copies or substantial           *
* portions of the Software.                                                                                      *
*                                                                                                                *
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT          *
* LIMITED TO THE WARRANTIES OF MERCHANTABILITY, * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.          *
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, * DAMAGES OR OTHER LIABILITY,      *
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE            *
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                                                         *
*****************************************************************************************************************/
#pragma once


#include <vector>
#include <stdlib.h>
#include <windows.h>
#include <math.h>
#include "types.h"
#include "timer.h"
#include "io.h"
#include "audio/midi/midi2.h"
#include "audio/output/interface.h"
#include "audio/output/directsound.h"

#include "audio/synth/base.h"
#include "audio/synth/types.h"
#include "audio/synth/misc.h"
#include "audio/synth/component.h"
#include "audio/synth/util.h"
#include "audio/synth/wavetable.h"
#include "audio/synth/synth.h"
#include "audio/synth/adsr.h"

#include "audio/synth/filters/lowpass.h"
#include "audio/synth/filters/chebyshev_filter.h"
#include "audio/synth/filters/iir_filter.h"
#include "audio/synth/effects/effect.h"
#include "audio/synth/effects/delay.h"

#include "audio/synth/oscillators/oscillator.h"
#include "audio/synth/oscillators/sine_oscillator.h"
#include "audio/synth/oscillators/triangle_oscillator.h"
#include "audio/synth/oscillators/sawtooth_oscillator.h"
#include "audio/synth/oscillators/square_oscillator.h"
#include "audio/synth/oscillators/exp_oscillator.h"

#include "audio/synth/instruments/instrument.h"
#include "audio/synth/instruments/percussion.h"
#include "audio/synth/instruments/instrument.h"
#include "audio/synth/instruments/osc_wave.h"
//#include "audio/synth/instruments/pad.h"
//#include "audio/synth/instruments/piano.h"
//#include "audio/synth/instruments/violin.h"
#include "audio/synth/instruments/percussion.h"
#include "audio/synth/instruments/blit.h"
#include "audio/synth/instruments/karplusstrong.h"
//#include "audio/synth/instruments/sonant_program.h"
//#include "audio/synth/instruments/waveguide_synthesis.h"
#include "audio/synth/channel.h"
#include "audio/synth/midi_synth.h"
#include "audio/synth/wave_synth.h"
#include "audio/formats/spc.h"

#include "audio/synth/player.h"
