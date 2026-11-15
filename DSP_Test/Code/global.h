#define NOMINMAX
#include <stdint.h>
#include <windows.h>
#include <stdio.h>
#include <math.h>
#include <commdlg.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <vector>
// OpenGL visualiser (gl.h/gl.cpp, needs GLEW) is not part of the build for now.
#include "..\..\DSP_Lib\Code\dsp.h"
#include "gfx.h"
#include "ui.h"
#include "client_main_window.h"
