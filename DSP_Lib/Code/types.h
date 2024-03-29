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
#include <inttypes.h>
#include <stddef.h>

#define null 0

#define _2_POW_12TH 1.0594630943592952645618252949463f
#define _LN_2 0.69314718055994530941723212145818f
#define _LN_2_DIV_12 0.05776226504666210911810267678818f
#define XM_PI  3.14159265358979323846f

namespace dsp {

template<typename T1,typename T2>
union AnyCast{
  T1 in;
  T2 out;
};

typedef float real_t;

typedef AnyCast<uint32_t,real_t> cast_uint32_real_t;


template<class Interface> 
inline void SafeRelease(Interface **ppInterfaceToRelease) {
    if (*ppInterfaceToRelease != NULL) {
        (*ppInterfaceToRelease)->Release();
        (*ppInterfaceToRelease) = NULL;
    }
}

template<class Interface> 
inline void SafeDelete(Interface **ppInterfaceToDelete) {
    if (*ppInterfaceToDelete != NULL) {
        delete (*ppInterfaceToDelete);
        (*ppInterfaceToDelete) = NULL;
    }
}

template<class Interface> 
inline void SafeDeleteArray(Interface **ppInterfaceToDelete) {
    if (*ppInterfaceToDelete != NULL) {
        delete [] (*ppInterfaceToDelete);
        (*ppInterfaceToDelete) = NULL;
    }
}

}